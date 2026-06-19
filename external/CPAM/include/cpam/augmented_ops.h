#pragma once
#include "map_ops.h"
#include "utils.h"

// *******************************************
//   AUGMENTED MAP OPERATIONS
// *******************************************

namespace cpam {

template<class Map>
struct augmented_ops : Map
{
  using Entry = typename Map::Entry;
  using node = typename Map::node;
  using Seq = typename Map::_Seq;
  using ET = typename Map::ET;
  using GC = typename Map::GC;
  using K = typename Map::K;
  using aug_t = typename Entry::aug_t;
  using ptr = typename GC::ptr;
  using Map::B;
  using Map::kBaseCaseSize;
  using Map::kNodeLimit;

  static inline aug_t aug_val(node* b)
  {
    return Seq::aug_val(b);
  }

  struct aug_sum_t
  {
    aug_t result;
    aug_sum_t()
      : result(Entry::get_empty())
    {
    }
    void add_entry(ET e)
    {
      result = Entry::combine(result, Entry::from_entry(e));
    }
    void add_aug_val(aug_t av)
    {
      result = Entry::combine(result, av);
    }
  };

  // the sum left of or at key
  template<class aug>
  static void aug_sum_left(node* b, K const& key, aug& a)
  {
    while (b) {
      if (Map::is_compressed(b)) {
        auto fn = [&](auto const& et) {
          if (Map::comp(Entry::get_key(et), key)) {
            a.add_entry(et);
            return true;
          }
          return false;
        };
        Map::iterate_cond(b, fn);
        return;
      }
      auto rb = Map::cast_to_regular(b);
      if (!Map::comp(key, Map::get_key(rb))) {
        a.add_entry(Map::get_entry(rb));
        if (rb->lc)
          a.add_aug_val(Map::aug_val(rb->lc));
        b = rb->rc;
      } else {
        b = rb->lc;
      }
    }
  }

  // the sum right of or at key
  template<class aug>
  static void aug_sum_right(node* b, K const& key, aug& a)
  {
    while (b) {
      if (Map::is_compressed(b)) {
        auto fn = [&](auto const& et) {
          if (!Map::comp(Entry::get_key(et), key)) {
            a.add_entry(et);
          }
        };
        Map::iterate_seq(b, fn);
        return;
      }
      auto rb = Map::cast_to_regular(b);
      if (!Map::comp(Map::get_key(rb), key)) {
        a.add_entry(Map::get_entry(rb));
        if (rb->rc)
          a.add_aug_val(Map::aug_val(rb->rc));
        b = rb->lc;
      } else
        b = rb->rc;
    }
  }

  template<class aug>
  static void aug_sum_range(node* b,
                            K const& key_left,
                            K const& key_right,
                            aug& a)
  {
    node* r = Map::range_root_2(b, key_left, key_right);
    if (r) {
      if (Map::is_compressed(r)) {
        auto fn = [&](auto const& et) {
          if (Map::comp(key_left, Entry::get_key(et)) &&
              Map::comp(Entry::get_key(et), key_right)) {
            a.add_entry(et);
          }
        };
        Map::iterate_seq(r, fn);
      } else {
        auto rr = Map::cast_to_regular(r);
        // add in left side (right of or at key_left)
        aug_sum_right(rr->lc, key_left, a);
        // add in middle
        a.add_entry(Map::get_entry(rr));
        // add in right side (left of or at key_right)
        aug_sum_left(rr->rc, key_right, a);
      }
    }
  }

  template<typename Func>
  static std::optional<ET> aug_select(node* b, Func const& f)
  {
    if (!b)
      return {};
    if (Map::is_compressed(b)) {
      std::optional<ET> ret;
      auto fn = [&](auto const& et) {
        if (!f(Entry::from_entry(et))) {
          ret = et;
          return false; // stop
        }
        return true; // keep iterating
      };
      Map::iterate_cond(b, fn);
      return ret;
    }
    auto rb = Map::cast_to_regular(b);
    if (f(Map::aug_val(rb->lc))) {
      if (f(Entry::from_entry(Map::get_entry(rb)))) {
        return aug_select(rb->rc, f);
      }
      return Map::get_entry(rb);
    }
    return aug_select(rb->lc, f);
  }

  template<class Func>
  static node* aug_filter_bc(ptr b1, Func const& f)
  {
    assert(b1.size() > 0);
    ET stack[kBaseCaseSize + 1];

    auto b1_node = b1.node_ptr();
    size_t offset = 0;
    aug_t cur = Entry::get_empty();
    auto copy_f = [&](ET a) { // has to be a copy since we move
      cur = Entry::combine(cur, Entry::from_entry(a));
      if (f(cur)) {
        parlay::move_uninitialized(stack[offset++], a);
      }
    };
    Map::iterate_seq(b1_node, copy_f);
    assert(offset <= kBaseCaseSize);

    Map::decrement_recursive(b1_node);

    if (offset < B) {
      return Map::to_tree_impl((ET*)stack, offset);
    } else {
      return Map::make_compressed(stack, offset);
    }
  }

  template<class Func>
  static node* aug_filter(ptr b, Func const& f, size_t granularity = kNodeLimit)
  {
    if (b.empty())
      return NULL;
    if (b.size() <= kBaseCaseSize) {
      return aug_filter_bc(std::move(b), f);
    }
    // TODO: better functionality for getting aug_val from b
    // std::cout << "My aug_val = " << aug_val(b.unsafe_ptr()) << std::endl;
    if (!f(aug_val(b.unsafe_ptr())))
      return NULL;

    size_t n = b.size();
    auto [lc, e, rc, root] = Map::expose(std::move(b));

    auto [l, r] = utils::fork<node*>(
      n >= granularity,
      [&]() { return aug_filter(std::move(lc), f, granularity); },
      [&]() { return aug_filter(std::move(rc), f, granularity); });

    if (f(Entry::from_entry(e))) {
      return Map::join(l, e, r, root);
    } else {
      GC::decrement(root);
      return Map::join2(l, r);
    }
  }

  template<class Func>
  static node* insert_lazy(node* b, const ET& e, Func const& f)
  {
    aug_t av = Entry::from_entry(e);
    auto g = [&](aug_t const& a) { return Entry::combine(av, a); };

    auto lazy_join = [&](node* l, node* r, node* _m) -> node* {
      auto m = Map::cast_to_regular(_m);
      m->rc = r;
      m->lc = l;
      if (Map::is_balanced(m)) {
        Map::lazy_update(m, g);
        return m;
      } else
        return Map::node_join(l, r, m);
    };

    return Map::template insert_tmpl<Func, decltype(lazy_join), false>(
      b, e, f, lazy_join);
  }

  template<class F, typename Logger>
  static void range_count(ptr b, F const& f, int64_t& cnt, Logger& logger)
  {

    if (b.empty()) {
      return;
    }

    if (b.is_compressed()) { // leaf node
      logger.vis_leaf_num++;
      auto f_filter = [&](auto const& et) {
        auto key = std::get<0>(et);
        if (f(key) == 0) {
          cnt++;
        }
      };
      Map::iterate_seq(b.node_ptr(), f_filter);
      return;
    }

    logger.vis_interior_num++;
    // auto rb = Map::cast_to_regular(b);
    auto [lc, e, rc, root] = Seq::expose(std::move(b));
    auto cur_key = Entry::get_key(e);
    auto pos = f(cur_key);
    if (pos == -1) {
      range_count(std::move(rc), f, cnt, logger);
    } else if (pos == 1) {
      range_count(std::move(lc), f, cnt, logger);
    } else {
      auto entry = e;
      cnt++;
      range_count(std::move(lc), f, cnt, logger);
      range_count(std::move(rc), f, cnt, logger);
    }
    GC::decrement(root);
  }

  template<class F, typename Out, typename Logger>
  static void range_report_filter2(node* b,
                                   F const& f,
                                   int64_t& cnt,
                                   Out& out,
                                   Logger& logger)
  {

    if (!b) {
      return;
    }

    if (Map::is_compressed(b)) { // leaf node
      logger.vis_leaf_num++;
      // NOTE: binary search below
      // auto c = Seq::cast_to_compressed(b);
      // ET* ets = (ET*)(((uint8_t*)c) + sizeof(*c));
      // size_t s = c->s;

      // size_t lo, hi;
      // {
      //   size_t l = 0, r = s;
      //   while (l < r) {
      //     size_t m = l + ((r - l) >> 1);
      //     if (f(Entry::get_key(ets[m])) < 0)
      //       l = m + 1;
      //     else
      //       r = m;
      //   }
      //   lo = l;
      // }
      // {
      //   size_t l = lo, r = s;
      //   while (l < r) {
      //     size_t m = l + ((r - l) >> 1);
      //     if (f(Entry::get_key(ets[m])) <= 0)
      //       l = m + 1;
      //     else
      //       r = m;
      //   }
      //   hi = l;
      // }
      // for (size_t i = lo; i < hi; i++) {
      //   out[cnt++] = { Entry::get_key(ets[i]), Entry::get_val(ets[i]) };
      // }

      auto f_filter = [&](auto const& et) {
        auto key = std::get<0>(et);
        if (f(key) == 0) {
          out[cnt++] = { std::get<0>(et), std::get<1>(et) };
        }
      };
      Map::iterate_seq(b, f_filter);
      return;
    }

    logger.vis_interior_num++;
    auto rb = Map::cast_to_regular(b);
    auto const& e = rb->entry.first; // entry is (ET, aug); ET = (key, val)
    auto cur_key = Entry::get_key(e);
    auto pos = f(cur_key);
    if (pos == -1) {
      range_report_filter2(rb->rc, f, cnt, out, logger);
    } else if (pos == 1) {
      range_report_filter2(rb->lc, f, cnt, out, logger);
    } else {
      out[cnt++] = { Entry::get_key(e), Entry::get_val(e) };
      range_report_filter2(rb->lc, f, cnt, out, logger);
      range_report_filter2(rb->rc, f, cnt, out, logger);
    }
    // GC::decrement(root);
  }

  template<class F, typename Out, typename Logger>
  static void range_report_filter2(ptr b,
                                   F const& f,
                                   int64_t& cnt,
                                   Out& out,
                                   Logger& logger)
  {

    if (b.empty()) {
      return;
    }

    if (b.is_compressed()) { // leaf node
      logger.vis_leaf_num++;
      // The leaf stores its entries as a flat, key-sorted ET[] array (the
      // default_entry_encoder layout). f(key) is monotone over that order:
      // -1 below the query range, 0 inside, 1 above. So instead of scanning
      // and testing every entry, binary-search the in-range sub-array [lo, hi):
      //   lo = first index with f(key) >= 0  (key >= query left bound)
      //   hi = first index with f(key) == 1  (key >= query right bound)
      auto c = Seq::cast_to_compressed(b.node_ptr());
      ET* ets = (ET*)(((uint8_t*)c) + sizeof(*c));
      size_t s = c->s;

      size_t lo, hi;
      {
        size_t l = 0, r = s;
        while (l < r) {
          size_t m = l + ((r - l) >> 1);
          if (f(Entry::get_key(ets[m])) < 0)
            l = m + 1;
          else
            r = m;
        }
        lo = l;
      }
      {
        size_t l = lo, r = s;
        while (l < r) {
          size_t m = l + ((r - l) >> 1);
          if (f(Entry::get_key(ets[m])) <= 0)
            l = m + 1;
          else
            r = m;
        }
        hi = l;
      }
      for (size_t i = lo; i < hi; i++) {
        out[cnt++] = { Entry::get_key(ets[i]), Entry::get_val(ets[i]) };
      }
      return;
    }

    logger.vis_interior_num++;
    // auto rb = Map::cast_to_regular(b);
    auto [lc, e, rc, root] = Seq::expose(std::move(b));
    auto cur_key = Entry::get_key(e);
    auto pos = f(cur_key);
    if (pos == -1) {
      range_report_filter2(std::move(rc), f, cnt, out, logger);
    } else if (pos == 1) {
      range_report_filter2(std::move(lc), f, cnt, out, logger);
    } else {
      auto entry = e;
      out[cnt++] = { Entry::get_key(e), Entry::get_val(e) };
      range_report_filter2(std::move(lc), f, cnt, out, logger);
      range_report_filter2(std::move(rc), f, cnt, out, logger);
    }
    GC::decrement(root);
  }
};

} // namespace cpam
