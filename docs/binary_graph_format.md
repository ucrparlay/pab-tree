# Binary Graph Header Format

This file format is used to store metadata about a graph in a compact, binary representation. The format is intended to be read using C++ `ifstream` with `.read()`.

## Header Structure (Total: 8 fields)

| Field              | Type       | Size (bytes) | Description                                                                 |
|-------------------|------------|--------------|-----------------------------------------------------------------------------|
| `num_vertices`     | `uint64_t` | 8            | Number of vertices in the graph.                                            |
| `num_unique_edges` | `uint64_t` | 8            | Number of unique edges.                                                    |
| `total_updates`    | `uint64_t` | 8            | Total number of edge insertions or updates in the file.                    |
| `if_signal_graph`  | `uint8_t`  | 1            | Boolean flag (0 = false, 1 = true) indicating if the graph is a signal graph. |
| `vertex_id_type`   | `uint8_t`  | 1            | Enum value from `DataType` indicating the type of vertex IDs.              |
| `timestamp_type`   | `uint8_t`  | 1            | Enum value from `DataType` indicating the type used for timestamps.        |
| `if_directed`      | `uint8_t`  | 1            | Boolean flag (0 = false, 1 = true) indicating if the graph is directed.    |
| `weight_type`      | `uint8_t`  | 1            | Enum value from `DataType` indicating the edge weight type.                |

### Total Header Size

`8 + 8 + 8 + 1 + 1 + 1 + 1 + 1 = 29 bytes`
We pad it out to 32 bytes for future proofing and for alignment reasong 

---

## `DataType` Enum

Values for `vertex_id_type`, `timestamp_type`, and `weight_type` are encoded using the following `uint8_t` enum:

| Name      | Value | C++ Type   |
|-----------|--------|------------|
| `NONE`    | `0`    | *(not present)* |
| `UINT8`   | `10`   | `uint8_t`  |
| `UINT16`  | `11`   | `uint16_t` |
| `UINT32`  | `12`   | `uint32_t` |
| `UINT64`  | `13`   | `uint64_t` |
| `INT8`    | `20`   | `int8_t`   |
| `INT16`   | `21`   | `int16_t`  |
| `INT32`   | `22`   | `int32_t`  |
| `INT64`   | `23`   | `int64_t`  |
| `FLOAT32` | `30`   | `float`    |
| `FLOAT64` | `31`   | `double`   |

---

## Notes

- Fields like `timestamp_type` and `weight_type` can be set to `NONE` (`0`) if not used.
- If `if_signal_graph` is true, the graph may encode dynamic signals or updates; additional fields may follow in the data section, depending on graph format.
- This header is followed by graph data in a format specific to the selected data types.


# Data Row Format

Each row in the data section represents a graph update (e.g., an edge insertion). The layout of each row is determined by the header's type parameters.

Each row contains:

| Field         | Type (from header) | Present When                        | Description                                 |
|---------------|--------------------|-------------------------------------|---------------------------------------------|
| `src_id`      | `vertex_id_type`   | Always                              | Source vertex ID.                           |
| `dest_id`     | `vertex_id_type`   | Always                              | Destination vertex ID.                      |
| `timestamp`   | `timestamp_type`   | `timestamp_type != NONE`            | Logical timestamp of the update.            |
| `weight`      | `weight_type`      | `weight_type != NONE`               | Weight of the edge.                         |
| `info`        | `UpdateInfo`       | `if_signal_graph == false`          | Additional metadata about the update.       |

### Notes:

- Fields appear **in the order shown** and are tightly packed with no padding.
- The presence of `timestamp`, `weight`, and `info` fields depends on the header:
  - `timestamp_type == NONE` → no `timestamp` field present
  - `weight_type == NONE` → no `weight` field present
  - `if_signal_graph == true` → no `info` field present
- All values are stored in **binary format** using little-endian byte order.

### Example Layouts

#### Example 1: Unweighted, Untimed Directed Graph
Header:
- `vertex_id_type = UINT32`
- `timestamp_type = NONE`
- `weight_type = NONE`
- `if_signal_graph = true`

Each row:
```c
uint32_t src_id;
uint32_t dest_id;
```

#### Example 2: Weighted, Timestamped, Undirected Graph
Header:

- `vertex_id_type = UINT64`

- `timestamp_type = UINT32`

- `weight_type = FLOAT32`

- `if_signal_graph = false`

Each row:

```c
uint64_t src_id;
uint64_t dest_id;
uint32_t timestamp;
float    weight;
UpdateInfo info; // 1 byte usually used to say insert of delete
```
