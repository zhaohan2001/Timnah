import numpy as np
import matplotlib.pyplot as plt

# Set the Zipfian alpha parameter
alpha = 1.5
n = 100  # Number of items

# Generate Zipfian probabilities
rank = np.arange(1, n+1)
frequency = 1 / rank**alpha

# Normalize frequencies to sum to one
frequency /= frequency.sum()

# Draw the bar chart
plt.figure(figsize=(10,6))
plt.bar(rank, frequency, color='skyblue', label=f'α = {alpha}')

plt.title("Zipfian Distribution Frequencies (α = 1.5)")
plt.xlabel("Rank")
plt.ylabel("Frequency")
plt.grid(True, axis='y', linestyle='--', alpha=0.7)
plt.legend()

# Save the image as PNG
plt.savefig("zipfian_distribution_bar_alpha_1.5.png", format="png", dpi=300)

# Show the bar chart
plt.show()