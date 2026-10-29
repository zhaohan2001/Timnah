import subprocess
import re
import matplotlib.pyplot as plt

# Define the executable and fixed arguments
binary_path = "./ycsb"
base_args = ["-index=Timnah", "-num_rounds=10", "-num_bg=1"]

# Requested thread counts
thread_counts = [5, 10, 15, 20, 25, 30, 35, 40, 45]

# Store thread counts and their round-25 throughput
results = []

# Match round 25 and its run-throughput line
round_pattern = re.compile(r"Round (\d+)")
throughput_pattern = re.compile(r"(?:Run throughput|\u8fd0\u884c\u541e\u5410): ([\d.]+) Mops/s")

for num_threads in thread_counts:
    print(f"Running with {num_threads} threads...")

    # Build the command
    cmd = [binary_path] + base_args + [f"-num_threads={num_threads}"]
    
    # Run the command and capture its output
    result = subprocess.run(cmd, capture_output=True, text=True)
    
    output_lines = result.stdout.splitlines()
    
    # Track the current round
    current_round = None
    round_25_throughput = None
    
    # Process each output line
    for line in output_lines:
        # Check for a new round
        round_match = round_pattern.search(line)
        if round_match:
            current_round = int(round_match.group(1))
        
        # Find throughput while processing round 25
        if current_round == 25:
            throughput_match = throughput_pattern.search(line)
            if throughput_match:
                round_9_throughput = float(throughput_match.group(1))
                break  # Stop after finding round-25 throughput
    
    if round_9_throughput is not None:
        print(f"Threads: {num_threads}, Round 25 Throughput: {round_9_throughput} Mops/s")
        results.append((num_threads, round_9_throughput))
    else:
        print(f"Error: Could not find Round 25 throughput for {num_threads} threads.")

# Print the final results
print("Collected data for Round 25:")
for num_threads, throughput in results:
    print(f"Threads: {num_threads}, Round 25 Throughput: {throughput} Mops/s")

# Extract thread counts and throughput for plotting
thread_counts, throughputs = zip(*results)

# Draw the chart
plt.figure(figsize=(6, 4))  # Set the figure size
plt.plot(thread_counts, throughputs, color='blue', marker='s', linestyle='-', label='Benchmark', markersize=8, linewidth=2)

# Chart styling
plt.xlabel("Number of Client Threads", fontsize=12)  # X-axis label
plt.ylabel("Throughput (Mops)", fontsize=12)         # Y-axis label
plt.title("Throughput vs Number of Client Threads", fontsize=14)  # Title
plt.grid(True, linestyle='--', alpha=0.7)  # Grid lines
plt.xlim(0, max(thread_counts) + 10)       # X-axis range
plt.ylim(0, max(throughputs) + 10)         # Y-axis range

# Configure the legend
plt.legend(loc='best', fontsize=10)

# Save the chart to an image file
plt.savefig("throughput_vs_threads.png", dpi=300)

# Show the chart
plt.show()

