#!/bin/bash
#
# OPTIMIZED OSM Data Processing Pipeline
# This script runs the complete OSM data processing workflow:
# 1. Convert OSM PBF to CSV (extract nodes with id, timestamp, lon, lat)
# 2. Filter by year (2014-2024) FIRST - removes unwanted records early
# 3. Shift coordinates, compute Morton codes, remove duplicates, and split into yearly files
#
# This optimized order processes Morton codes only for records we keep!

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
	echo "Usage: $0 <input.osm.pbf> [output_dir] [output_prefix]"
	echo ""
	echo "Arguments:"
	echo "  input.osm.pbf  - Input OSM PBF file (required)"
	echo "  output_dir     - Directory for output files (optional, default: current directory)"
	echo "  output_prefix  - Prefix for output files (optional, default: osm_output)"
	echo ""
	echo "Examples:"
	echo "  $0 file.osm.pbf"
	echo "    → Outputs to current directory with prefix 'osm_output'"
	echo ""
	echo "  $0 file.osm.pbf /data/results"
	echo "    → Outputs to /data/results/ with prefix 'osm_output'"
	echo ""
	echo "  $0 file.osm.pbf /data/results central_america"
	echo "    → Outputs to /data/results/ with prefix 'central_america'"
	echo ""
	echo "This will (OPTIMIZED ORDER):"
	echo "  1. Convert OSM PBF to CSV"
	echo "  2. Filter by year (2014-2024) - removes unwanted records early!"
	echo "  3. Compute Morton codes ONLY for filtered records and split into yearly files"
	exit 1
fi

INPUT_FILE="$1"
OUTPUT_DIR="${2:-.}"
OUTPUT_PREFIX="${3:-osm_output}"

# Check if input file exists
if [ ! -f "$INPUT_FILE" ]; then
	log_error "Input file not found: $INPUT_FILE"
	exit 1
fi

# Create output directory if it doesn't exist
if [ ! -d "$OUTPUT_DIR" ]; then
	log_info "Creating output directory: $OUTPUT_DIR"
	mkdir -p "$OUTPUT_DIR"
fi

# Convert to absolute paths
OUTPUT_DIR="$(cd "$OUTPUT_DIR" && pwd)"

# Get the directory where this script is located
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Define intermediate file names
CSV_FILE="${INPUT_FILE%.osm.pbf}.osm.pbf.csv"
FILTERED_FILE="${INPUT_FILE%.osm.pbf}.osm.pbf_filtered.csv"

log_info "=========================================="
log_info "OPTIMIZED OSM Data Processing Pipeline"
log_info "=========================================="
log_info "Input file: $INPUT_FILE"
log_info "Output directory: $OUTPUT_DIR"
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
# Step 2: Filter by year (2014-2024) - NEW OPTIMIZED STEP!
# ==========================================
log_info "Step 2/3: Filtering by year (2014-2024)..."
log_info "Output: $FILTERED_FILE"
log_info "This reduces data size before expensive Morton computation!"

if [ -f "$FILTERED_FILE" ]; then
	log_warning "Filtered file already exists: $FILTERED_FILE"
	read -p "Do you want to skip this step? (y/n) " -n 1 -r
	echo
	if [[ $REPLY =~ ^[Yy]$ ]]; then
		log_info "Skipping Step 2"
	else
		log_info "Running filter_year_range.py..."
		python3 "$SCRIPT_DIR/filter_year_range.py" "$CSV_FILE" "$FILTERED_FILE"
		log_info "Step 2 complete!"
	fi
else
	log_info "Running filter_year_range.py..."
	python3 "$SCRIPT_DIR/filter_year_range.py" "$CSV_FILE" "$FILTERED_FILE"
	log_info "Step 2 complete!"
fi

echo ""

# ==========================================
# Step 3: Compute Morton codes and split by year
# ==========================================
log_info "Step 3/3: Computing Morton codes and splitting into yearly files..."
log_info "Processing ONLY the filtered records (much faster!)"
log_info "Output directory: $OUTPUT_DIR"
log_info "Output prefix: $OUTPUT_PREFIX"

log_info "Running compute_morton_and_split.py..."
python3 "$SCRIPT_DIR/compute_morton_and_split.py" "$FILTERED_FILE" "$OUTPUT_DIR" "$OUTPUT_PREFIX"
log_info "Step 3 complete!"

echo ""

# ==========================================
# Pipeline Complete
# ==========================================
log_info "=========================================="
log_info "Pipeline Complete!"
log_info "=========================================="
log_info "Generated files:"
log_info "  1. Raw CSV (all records): $CSV_FILE"
log_info "  2. Filtered (2014-2024 only): $FILTERED_FILE"
log_info "  3. Yearly files with Morton codes: $OUTPUT_DIR/${OUTPUT_PREFIX}_2014.csv to ${OUTPUT_PREFIX}_2024.csv"
echo ""

# Show summary of yearly files
log_info "Yearly file summary:"
ls -lh "$OUTPUT_DIR/${OUTPUT_PREFIX}"_*.csv 2>/dev/null || log_warning "No yearly files found"

log_info ""
log_info "All done!"
log_info ""
log_info "OPTIMIZATION: Morton codes computed only for filtered records!"
log_info "This saves significant processing time compared to the old approach."
