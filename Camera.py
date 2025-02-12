import subprocess
import datetime
import os

VIDEO_DURATION = 60  # Aqui solo se pondrian 3600 segundos para que grabe 1 hora continua
FINAL_VIDEO_DURATION = 10  # aqui se pondrian los 30 o 45 o lo que sea segundos que vayamos a guardar para ellos
webcam_input = "video=Integrated Webcam"
log_filename = "timestamps.log"

def read_log_file():
    """Reads timestamps from the log file and returns them as datetime objects."""
    if os.path.exists(log_filename):
        with open(log_filename, 'r') as file:
            dates = []
            for line in file:
                try:
                    date_entry = datetime.datetime.strptime(line.strip(), "%Y-%m-%d_%H-%M-%S")
                    dates.append(date_entry)
                except ValueError:
                    print(f"Skipping invalid date line: {line.strip()}")
            return dates
    else:
        return []

def extract_clip(full_video, video_start_time, clip_timestamp):
    """Extracts a FINAL_VIDEO_DURATION-second clip ending at clip_timestamp."""
    # Calculate the clip's start seconds relative to the beginning of the video
    clip_start_seconds = (clip_timestamp - video_start_time).total_seconds() - FINAL_VIDEO_DURATION

    print(f"############### {clip_timestamp} ###################")
    print(f"############### Clip start time (relative): {clip_start_seconds} ###################")

    if clip_start_seconds < 0:
        print(f"❌ Error: Clip start time {clip_start_seconds}s is before the start of the video!")
        return

    clip_name = f"{clip_timestamp.strftime('%Y-%m-%d_%H-%M-%S')}.mp4"

    ffmpeg_command = [
        "ffmpeg",
        "-i", full_video,                 # Load the input file
        "-ss", str(clip_start_seconds),   # Seek to the start time
        "-t", str(FINAL_VIDEO_DURATION),  # Extract exactly FINAL_VIDEO_DURATION
        "-c:v", "libx264", "-preset", "ultrafast",  # Re-encode for precision
        "-c:a", "aac", "-b:a", "128k",  # Ensure audio is included
        "-y",  # Overwrite existing files
        clip_name
    ]

    print(f"Extracting {FINAL_VIDEO_DURATION}-second clip: {clip_name}")
    result = subprocess.run(ffmpeg_command, capture_output=True, text=True)
    print(result.stdout)
    print(result.stderr)

    if "Output file is empty" in result.stderr:
        print(f"❌ Extraction failed: No frames found in {clip_name}")

    print(f"✅ Clip saved: {clip_name}")




def process_video(video_filename, video_start_time):
    """Processes the recorded video: extracts necessary clips and deletes the full video."""
    log_dates = read_log_file()
    video_end_time = video_start_time + datetime.timedelta(seconds=VIDEO_DURATION)

    extracted = False
    for log_date in log_dates:
        if video_start_time <= log_date <= video_end_time:
            extract_clip(video_filename, video_start_time, log_date)
            extracted = True

    # Delete the full video after extracting clips
    os.remove(video_filename)
    print(f"Deleted full video: {video_filename}")


def record_video():
    """Continuously records videos and processes them after recording."""
    try:
        while True:
            start_time = datetime.datetime.now()

            ffmpeg_command = [
                "ffmpeg",
                "-f", "dshow",  
                "-rtbufsize", "150M", #probablemente ocupemos mas buffer para videos de 1 hora
                "-c:a", "aac",
                "-i", webcam_input,
                "-t", str(VIDEO_DURATION),
                "-y",  # Overwrite output file if it exists
                "temp_video.mp4"
            ]

            print(f"Recording video for {VIDEO_DURATION} seconds...")
            subprocess.run(ffmpeg_command, check=True)

            # Save the recorded video with a timestamped filename
            end_time = datetime.datetime.now()
            filename = f"{end_time.strftime('%Y-%m-%d_%H-%M-%S')}.mp4"
            os.rename("temp_video.mp4", filename)
            print(f"Recording complete. Video saved as {filename}")

            # Process the recorded video
            process_video(filename, start_time)

    except subprocess.CalledProcessError as e:
        print(f"An error occurred while recording the video: {e}")
    except FileNotFoundError as e:
        print(f"Error renaming file: {e}")
    except KeyboardInterrupt:
        print("Recording interrupted by user.")
        try:
            final_timestamp = datetime.datetime.now().strftime("%Y-%m-%d_%H-%M-%S")
            final_filename = f"{final_timestamp}.mp4"
            os.rename("temp_video.mp4", final_filename)
            print(f"Final video saved as {final_filename}")
            process_video(final_filename, datetime.datetime.now())
        except FileNotFoundError:
            print("No final video to save.")

if __name__ == "__main__":
    try:
        record_video()
    except KeyboardInterrupt:
        print("Exiting the recording loop.")
