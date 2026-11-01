#!/bin/sh
# Creates the input files for "make test" (requires the ffmpeg command-line tool).
#   usage: make_fixtures.sh PATH/TO/test_colors OUTPUT_DIR
set -e
TEST_COLORS=$1
OUT=$2
command -v ffmpeg >/dev/null || { echo "The tests need ffmpeg (e.g. sudo apt install ffmpeg)"; exit 1; }
mkdir -p "$OUT"
cd "$OUT"
[ -f .done ] && exit 0

FF="ffmpeg -hide_banner -loglevel error -y"

# Video for the receiver test: 40 frames at 640x480 without B-frames, like AirPlay
$FF -f lavfi -i testsrc=size=640x480:rate=30 -frames:v 40 -c:v libx264 -pix_fmt yuv420p \
    -bf 0 -g 20 -bsf:v h264_mp4toannexb land.h264

# AAC-LC audio, 44.1 kHz stereo (440 Hz tone)
$FF -f lavfi -i sine=frequency=440:sample_rate=44100:duration=1 -ac 2 -c:a aac -b:a 128k -f adts sine.aac

# Image with known colors, losslessly encoded in three color formats
"$TEST_COLORS" --pattern pattern.ppm
encode() { # name matrix range range_tag transfer
    $FF -loop 1 -i pattern.ppm -frames:v 3 \
        -vf "scale=out_color_matrix=$2:out_range=$3,format=yuv420p" \
        -c:v libx264 -qp 0 -bf 0 -color_range "$4" -colorspace "$2" -color_primaries "$2" \
        -color_trc "$5" -bsf:v h264_mp4toannexb "$1.h264"
}
encode full709 bt709     full    pc iec61966-2-1   # like Apple's stream
encode lim709  bt709     limited tv bt709
encode lim601  smpte170m limited tv smpte170m

touch .done
