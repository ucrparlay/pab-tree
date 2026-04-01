#!/usr/bin/env python3
"""
Extract node IDs and timestamps from OSM PBF file.

This script reads an OSM PBF file and outputs a CSV with:
- id: Node ID
- ts: Timestamp

Output format: CSV with header (id,ts)
"""

import sys
import osmium
import csv


class NodeHandler(osmium.SimpleHandler):
    """Handler to extract node IDs and timestamps."""
    
    def __init__(self, csv_writer):
        osmium.SimpleHandler.__init__(self)
        self.csv_writer = csv_writer
        self.count = 0
    
    def node(self, n):
        """Process each node."""
        self.count += 1
        
        # Write: id, timestamp
        self.csv_writer.writerow([
            n.id,
            n.timestamp
        ])
        
        # Progress update every 1 million records
        if self.count % 1000000 == 0:
            print(f"  Processed {self.count:,} nodes...", file=sys.stderr)


def main():
    if len(sys.argv) < 2:
        print("Usage: python osm_to_csv_simple.py <input.osm.pbf>")
        print("\nExtracts node IDs and timestamps to CSV")
        print("Output: <input>.csv")
        sys.exit(1)
    
    input_file = sys.argv[1]
    output_file = input_file + '.csv'
    
    print(f"Reading: {input_file}")
    print(f"Writing: {output_file}")
    print()
    
    with open(output_file, 'w', newline='', encoding='utf-8') as f:
        writer = csv.writer(f)
        # Write header
        writer.writerow(['id', 'ts'])
        
        # Create handler and process file
        handler = NodeHandler(writer)
        handler.apply_file(input_file)
        
        print()
        print(f"Complete! Extracted {handler.count:,} nodes")
        print(f"Output: {output_file}")


if __name__ == '__main__':
    main()
