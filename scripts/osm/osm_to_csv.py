#!/usr/bin/env python3
"""
Convert OSM PBF file to CSV format.
Based on the reference implementation from Pkd-tree/script/osm.py
"""

import osmium as osm
import sys


MIN_YEAR = 2014
MAX_YEAR = 2024


class OSMHandler(osm.SimpleHandler):
    def __init__(self, output_file):
        osm.SimpleHandler.__init__(self)
        self.output_file = output_file
        self.node_count = 0

        # Write CSV header
        self.output_file.write("id,ts,lon,lat\n")

    def node(self, n):
        """Process each node in the OSM file."""
        try:
            if not (MIN_YEAR <= n.timestamp.year <= MAX_YEAR):
                return

            # Write node data: id, timestamp, longitude, latitude
            self.output_file.write(
                f"{n.id},{n.timestamp},{n.location.lon},{n.location.lat}\n"
            )

            self.node_count += 1
            if self.node_count % 1000000 == 0:
                print(
                    f"Processed {self.node_count:,} nodes...", file=sys.stderr)
        except Exception as e:
            # Skip nodes without valid location
            pass


def main():
    if len(sys.argv) < 2:
        print("Usage: python osm_to_csv.py <input.osm.pbf> [output.csv]")
        sys.exit(1)

    input_file = sys.argv[1]

    # Determine output file name
    if len(sys.argv) >= 3:
        output_file = sys.argv[2]
    else:
        output_file = input_file.replace('.osm.pbf', '.osm.pbf.csv')

    print(f"Converting {input_file} to CSV format...")
    print(f"Output file: {output_file}")
    print()

    # Open output file
    with open(output_file, 'w', encoding='utf-8') as f:
        handler = OSMHandler(f)

        # Process the OSM file
        print("Processing OSM data...")
        handler.apply_file(input_file)

        print()
        print("Conversion complete!")
        print(f"Total nodes: {handler.node_count:,}")
        print(f"Output saved to: {output_file}")


if __name__ == '__main__':
    main()
