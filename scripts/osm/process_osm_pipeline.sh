#!/bin/bash
#
# OSM Data Processing Pipeline
# This script runs the complete OSM data processing workflow:
# 1. Convert OSM PBF to CSV (extract nodes with id, timestamp, lon, lat)
# 2. Shift coordinates to positive range and compute Morton codes
# 3. Filter by year (2014-2024), remove duplicates, and split into yearly files
#
# Most common usage:
# # Go to where you want the output files
# cd /where/you/want/output
# # Run the script with full path to input
# /Users/ziyang/code/tmp/process_osm_pipeline.sh /path/to/input.osm.pbf my_output

set -e # Exit on any error

# Color output for better readability
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Function to print colored messages
log_info() {
	echo -e "${GREEN}[INFO]${NC} $1"
}

log_error() {
	echo -e "${RED}[ERROR]${NC} $1"
}

log_warning() {
	echo -e "${YELLOW}[WARNING]${NC} $1"
}

# Check if input file is provided
if [ $# -lt 1 ]; then
	echo "Usage: $0 <input.osm.pbf> [output_prefix]"
	echo ""
	echo "Example: $0 central-america-latest.osm.pbf osm_data"
	echo ""
	echo "This will:"
	echo "  1. Convert OSM PBF to CSV"
	echo "  2. Shift coordinates and compute Morton codes"
	echo "  3. Filter by year (2014-2024) and split into yearly files"
	exit 1
fi

INPUT_FILE="$1"
OUTPUT_PREFIX="${2:-osm_output}"

# Check if input file exists
if [ ! -f "$INPUT_FILE" ]; then
	log_error "Input file not found: $INPUT_FILE"
	exit 1
fi

# Get the directory where this script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Define intermediate file names
CSV_FILE="${INPUT_FILE%.osm.pbf}.osm.pbf.csv"
SHIFTED_FILE="${INPUT_FILE%.osm.pbf}.osm.pbf_shifted_morton.csv"

log_info "=========================================="
log_info "OSM Data Processing Pipeline"
log_info "=========================================="
log_info "Input file: $INPUT_FILE"
log_info "Output prefix: $OUTPUT_PREFIX"
log_info ""

# ==========================================
# Check prerequisites
# ==========================================
# Check if osmium is available in system Python or venv
if python3 -c "import osmium" 2>/dev/null; then
	log_info "osmium found in system Python"
	USE_VENV=false
elif [ -d "$SCRIPT_DIR/venv" ]; then
	log_info "Using virtual environment for osmium"
	USE_VENV=true
else
	log_error "osmium not found!"
	log_error "Please install it:"
	log_error "  Option 1 - System-wide: pip3 install osmium"
	log_error "  Option 2 - Virtual env:"
	log_error "    cd $SCRIPT_DIR"
	log_error "    python3 -m venv venv"
	log_error "    source venv/bin/activate"
	log_error "    pip install osmium"
	log_error "    deactivate"
	exit 1
fi

# ==========================================
# Step 1: Convert OSM PBF to CSV
# ==========================================
log_info "Step 1/3: Converting OSM PBF to CSV..."
log_info "Output: $CSV_FILE"

if [ -f "$CSV_FILE" ]; then
	log_warning "CSV file already exists: $CSV_FILE"
	read -p "Do you want to skip this step? (y/n) " -n 1 -r
	echo
	if [[ $REPLY =~ ^[Yy]$ ]]; then
		log_info "Skipping Step 1"
	else
		log_info "Running osm_to_csv.py..."
		if [ "$USE_VENV" = true ]; then
			source "$SCRIPT_DIR/venv/bin/activate"
			python "$SCRIPT_DIR/osm_to_csv.py" "$INPUT_FILE"
			deactivate
		else
			python3 "$SCRIPT_DIR/osm_to_csv.py" "$INPUT_FILE"
		fi
		log_info "Step 1 complete!"
	fi
else
	log_info "Running osm_to_csv.py..."
	if [ "$USE_VENV" = true ]; then
		source "$SCRIPT_DIR/venv/bin/activate"
		python "$SCRIPT_DIR/osm_to_csv.py" "$INPUT_FILE"
		deactivate
	else
		python3 "$SCRIPT_DIR/osm_to_csv.py" "$INPUT_FILE"
	fi
	log_info "Step 1 complete!"
fi

echo ""

# ==========================================
# Step 2: Shift coordinates and compute Morton codes
# ==========================================
log_info "Step 2/3: Shifting coordinates and computing Morton codes..."
log_info "Output: $SHIFTED_FILE"

if [ -f "$SHIFTED_FILE" ]; then
	log_warning "Shifted file already exists: $SHIFTED_FILE"
	read -p "Do you want to skip this step? (y/n) " -n 1 -r
	echo
	if [[ $REPLY =~ ^[Yy]$ ]]; then
		log_info "Skipping Step 2"
	else
		log_info "Running shift_coordinates.py..."
		python3 "$SCRIPT_DIR/shift_coordinates.py" "$CSV_FILE" "$SHIFTED_FILE"
		log_info "Step 2 complete!"
	fi
else
	log_info "Running shift_coordinates.py..."
	python3 "$SCRIPT_DIR/shift_coordinates.py" "$CSV_FILE" "$SHIFTED_FILE"
	log_info "Step 2 complete!"
fi

echo ""

# ==========================================
# Step 3: Filter by year and split into files
# ==========================================
log_info "Step 3/3: Filtering by year (2014-2024) and splitting into yearly files..."
log_info "Output: ${OUTPUT_PREFIX}_YYYY.csv"

log_info "Running filter_by_year.py..."
python3 "$SCRIPT_DIR/filter_by_year.py" "$SHIFTED_FILE" "$OUTPUT_PREFIX"
log_info "Step 3 complete!"

echo ""

# ==========================================
# Pipeline Complete
# ==========================================
log_info "=========================================="
log_info "Pipeline Complete!"
log_info "=========================================="
log_info "Generated files:"
log_info "  1. Raw CSV: $CSV_FILE"
log_info "  2. Shifted with Morton: $SHIFTED_FILE"
log_info "  3. Yearly files: ${OUTPUT_PREFIX}_2014.csv to ${OUTPUT_PREFIX}_2024.csv"
echo ""

# Show summary of yearly files
log_info "Yearly file summary:"
ls -lh ${OUTPUT_PREFIX}_*.csv 2>/dev/null || log_warning "No yearly files found"

log_info ""
log_info "All done!"
