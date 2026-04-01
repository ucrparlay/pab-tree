# OSM By Year - Extract and Split OSM Data by Year

This folder contains scripts to process OpenStreetMap (OSM) PBF files and split them into yearly files (2014-2024) with duplicate ID removal.

## Scripts

- `process_osm_by_year.sh` - Main pipeline script
- `osm_to_csv_simple.py` - Extract node IDs and timestamps from PBF
- `filter_and_split_by_year.py` - Filter by year, deduplicate, and split

## Requirements

- Python 3
- `osmium` library: `pip install osmium` (or use venv - see below)

## Usage

```bash
# Basic usage (output to current directory)
./process_osm_by_year.sh /path/to/input.osm.pbf

# Specify output directory
./process_osm_by_year.sh /path/to/input.osm.pbf /path/to/output_dir

# Specify output directory AND custom prefix
./process_osm_by_year.sh /path/to/input.osm.pbf /path/to/output_dir my_prefix
```

## Output Format

Creates 10 yearly files (2014-2024) with format:
```
<num_records> 2
<id1> <year>
<id2> <year>
...
```

Example `osm_output_2015.csv`:
```
3 2
1001 2015
1002 2015
1003 2015
```

## Features

- Filters records to years 2014-2024 only
- Removes duplicate IDs (keeps first occurrence)
- Creates 10 output files (one per year, even if empty)
- No Morton codes - simple ID and year format

## Virtual Environment Setup (Optional)

If osmium is not installed system-wide:

```bash
cd osm_by_year
python3 -m venv venv
source venv/bin/activate
pip install osmium
deactivate
```

The pipeline script will automatically detect and use the venv.
