

import matplotlib.pyplot as plt
import numpy as np
from matplotlib.patches import Polygon
import seaborn as sns
import os

# Set style for elegant plots
plt.style.use('seaborn-v0_8-whitegrid')
sns.set_palette("husl")

# Domains and actual counts (not log scale)
domains = ["kolesa.kz", "mercadolibre.com", "pornbox.com", "hm.com", "media.guilded.gg"]
messages = [72, 47, 80, 118, 358]
tokens = [44959, 424838, 100139, 40616, 8959492]

# Data per model: attack graph time (seconds) and cost ($)
models = {
    "claude-sonnet-4": {
        "time": [16, 12, 13, 30, 38],
        "cost": [0.16, 0.09, 0.19, 0.13, 0.28]
    },
    "gpt-4o": {
        "time": [14, 9.4, 11, 20, 56],
        "cost": [0.12, 0.05, 0.12, 0.10, 0.19]
    },
    "o3": {
        "time": [16, 27, 22, 26, 32],
        "cost": [0.10, 0.05, 0.11, 0.09, 0.18]
    },
    "grok-4": {
        "time": [30, 27, 27, 36, 41],
        "cost": [0.15, 0.07, 0.15, 0.13, 0.27]
    }
}

# Elegant Multi-Line Plot with Actual Counts
fig1, ((ax1a, ax1b), (ax1c, ax1d)) = plt.subplots(2, 2, figsize=(18, 12))

# Time performance vs Message Count
colors = ['#D3D3D3', '#20B2AA', '#4C9A99', '#307FE2']  # Light gray, teal green, #4C9A99, #307FE2
markers = ['o', 's', '^', 'D']
line_styles = ['-', '--', ':', '-.']

for i, (model, data) in enumerate(models.items()):
    # Sort by message count for smooth curve
    sorted_indices = np.argsort(messages)
    x_sorted = np.array(messages)[sorted_indices]
    y_sorted = np.array(data["time"])[sorted_indices]
    
    # Create smooth curve
    ax1a.plot(x_sorted, y_sorted, color=colors[i], linewidth=3, 
               marker=markers[i], markersize=8, label=model, alpha=0.8,
               linestyle=line_styles[i])

ax1a.set_xlabel('Number of Messages in Log', fontsize=12, fontweight='bold')
ax1a.set_ylabel('Attack Graph Generation Time (s)', fontsize=12, fontweight='bold')
ax1a.set_title('LLM Performance: Time vs Message Count', fontsize=14, fontweight='bold')
ax1a.legend(fontsize=10, frameon=True, fancybox=True, shadow=True)
ax1a.grid(True, alpha=0.3)
ax1a.set_xscale('log')
ax1a.set_xlim(10, 400)

# Cost performance vs Message Count
for i, (model, data) in enumerate(models.items()):
    sorted_indices = np.argsort(messages)
    x_sorted = np.array(messages)[sorted_indices]
    y_sorted = np.array(data["cost"])[sorted_indices]
    
    ax1b.plot(x_sorted, y_sorted, color=colors[i], linewidth=3, 
               marker=markers[i], markersize=8, label=model, alpha=0.8,
               linestyle=line_styles[i])

ax1b.set_xlabel('Number of Messages in Log', fontsize=12, fontweight='bold')
ax1b.set_ylabel('Inference Cost ($)', fontsize=12, fontweight='bold')
ax1b.set_title('LLM Performance: Cost vs Message Count', fontsize=14, fontweight='bold')
ax1b.legend(fontsize=10, frameon=True, fancybox=True, shadow=True)
ax1b.grid(True, alpha=0.3)
ax1b.set_xscale('log')
ax1b.set_xlim(10, 400)

# Time performance vs Token Count
for i, (model, data) in enumerate(models.items()):
    sorted_indices = np.argsort(tokens)
    x_sorted = np.array(tokens)[sorted_indices]
    y_sorted = np.array(data["time"])[sorted_indices]
    
    ax1c.plot(x_sorted, y_sorted, color=colors[i], linewidth=3, 
               marker=markers[i], markersize=8, label=model, alpha=0.8,
               linestyle=line_styles[i])

ax1c.set_xlabel('Number of Tokens in Log', fontsize=12, fontweight='bold')
ax1c.set_ylabel('Attack Graph Generation Time (s)', fontsize=12, fontweight='bold')
ax1c.set_title('LLM Performance: Time vs Token Count', fontsize=14, fontweight='bold')
ax1c.legend(fontsize=10, frameon=True, fancybox=True, shadow=True)
ax1c.grid(True, alpha=0.3)
ax1c.set_xscale('log')
ax1c.set_xlim(10000, 10000000)

# Cost performance vs Token Count
for i, (model, data) in enumerate(models.items()):
    sorted_indices = np.argsort(tokens)
    x_sorted = np.array(tokens)[sorted_indices]
    y_sorted = np.array(data["cost"])[sorted_indices]
    
    ax1d.plot(x_sorted, y_sorted, color=colors[i], linewidth=3, 
               marker=markers[i], markersize=8, label=model, alpha=0.8,
               linestyle=line_styles[i])

ax1d.set_xlabel('Number of Tokens in Log', fontsize=12, fontweight='bold')
ax1d.set_ylabel('Inference Cost ($)', fontsize=12, fontweight='bold')
ax1d.set_title('LLM Performance: Cost vs Token Count', fontsize=14, fontweight='bold')
ax1d.legend(fontsize=10, frameon=True, fancybox=True, shadow=True)
ax1d.grid(True, alpha=0.3)
ax1d.set_xscale('log')
ax1d.set_xlim(10000, 10000000)

plt.tight_layout()
plt.show()

# Save the first plot
current_dir = os.getcwd()
plot1_path = os.path.join(current_dir, 'llm_performance_comparison_plots.png')
fig1.savefig(plot1_path, dpi=300, bbox_inches='tight', facecolor='white')
print(f"📊 First plot saved to: {plot1_path}")

# NEW: One comprehensive plot per model showing all metrics (MESSAGES ONLY)
fig2, ((ax2a, ax2b), (ax2c, ax2d)) = plt.subplots(2, 2, figsize=(18, 12))

# Colors and markers for the four metrics
metric_colors = ['#D3D3D3', '#20B2AA', '#4C9A99', '#307FE2']  # Light gray, teal green, #4C9A99, #307FE2
metric_markers = ['o', 's', '^', 'D']
metric_line_styles = ['-', '--', ':', '-.']  # Solid, dashed, dotted, dash-dot
metric_names = ['Time vs Messages', 'Cost vs Messages', 'Time vs Tokens', 'Cost vs Tokens']

# Create one comprehensive plot per model (using messages for x-axis)
for idx, (model, data) in enumerate(models.items()):
    ax = [ax2a, ax2b, ax2c, ax2d][idx]
    
    # Plot 1: Time vs Messages
    sorted_indices = np.argsort(messages)
    x_sorted = np.array(messages)[sorted_indices]
    y_sorted = np.array(data["time"])[sorted_indices]
    ax.plot(x_sorted, y_sorted, color=metric_colors[0], linewidth=3, 
            marker=metric_markers[0], markersize=8, label=metric_names[0], alpha=0.8,
            linestyle=metric_line_styles[0])
    
    # Plot 2: Cost vs Messages (use secondary y-axis)
    ax2 = ax.twinx()
    y_sorted_cost = np.array(data["cost"])[sorted_indices]
    ax2.plot(x_sorted, y_sorted_cost, color=metric_colors[1], linewidth=3, 
             marker=metric_markers[1], markersize=8, label=metric_names[1], alpha=0.8,
             linestyle=metric_line_styles[1])
    
    # Plot 3: Time vs Tokens (normalize tokens to message scale for comparison)
    sorted_indices_tokens = np.argsort(tokens)
    x_tokens_sorted = np.array(tokens)[sorted_indices_tokens]
    y_tokens_sorted = np.array(data["time"])[sorted_indices_tokens]
    # Normalize tokens to message scale for visualization
    x_tokens_norm = (x_tokens_sorted - min(tokens)) / (max(tokens) - min(tokens)) * (max(messages) - min(messages)) + min(messages)
    ax.plot(x_tokens_norm, y_tokens_sorted, color=metric_colors[2], linewidth=3, 
            marker=metric_markers[2], markersize=8, label=metric_names[2], alpha=0.8,
            linestyle=metric_line_styles[2])
    
    # Plot 4: Cost vs Tokens
    y_tokens_cost_sorted = np.array(data["cost"])[sorted_indices_tokens]
    ax2.plot(x_tokens_norm, y_tokens_cost_sorted, color=metric_colors[3], linewidth=3, 
             marker=metric_markers[3], markersize=8, label=metric_names[3], alpha=0.8,
             linestyle=metric_line_styles[3])
    
    # Set labels and title
    ax.set_xlabel('Number of Messages in Log', fontsize=12, fontweight='bold')
    ax.set_ylabel('Time (s)', fontsize=12, fontweight='bold', color=metric_colors[0])
    ax2.set_ylabel('Cost ($)', fontsize=12, fontweight='bold', color=metric_colors[1])
    ax.set_title(f'{model.upper()}: All Performance Metrics', fontsize=14, fontweight='bold')
    
    # Add legends
    lines1, labels1 = ax.get_legend_handles_labels()
    lines2, labels2 = ax2.get_legend_handles_labels()
    ax.legend(lines1 + lines2, labels1 + labels2, loc='upper left', fontsize=9, 
              frameon=True, fancybox=True, shadow=True)
    
    ax.grid(True, alpha=0.3)
    ax.set_xscale('log')
    ax.set_xlim(10, 400)
    
    # Add performance summary
    avg_time = np.mean(data["time"])
    avg_cost = np.mean(data["cost"])
    ax.text(0.02, 0.98, f'Avg Time: {avg_time:.1f}s\nAvg Cost: ${avg_cost:.2f}', 
            transform=ax.transAxes, fontsize=10, verticalalignment='top',
            bbox=dict(boxstyle="round,pad=0.3", facecolor="white", alpha=0.8))

plt.tight_layout()
plt.show()

# Save the second plot
plot2_path = os.path.join(current_dir, 'llm_comprehensive_performance_metrics.png')
fig2.savefig(plot2_path, dpi=300, bbox_inches='tight', facecolor='white')
print(f"📊 Second plot saved to: {plot2_path}")

# NEW: One comprehensive plot per model showing all metrics (TOKENS ONLY)
fig3, ((ax3a, ax3b), (ax3c, ax3d)) = plt.subplots(2, 2, figsize=(18, 12))

# Create one comprehensive plot per model (using tokens for x-axis)
for idx, (model, data) in enumerate(models.items()):
    ax = [ax3a, ax3b, ax3c, ax3d][idx]
    
    # Plot 1: Time vs Tokens
    sorted_indices_tokens = np.argsort(tokens)
    x_tokens_sorted = np.array(tokens)[sorted_indices_tokens]
    y_tokens_sorted = np.array(data["time"])[sorted_indices_tokens]
    ax.plot(x_tokens_sorted, y_tokens_sorted, color=metric_colors[0], linewidth=3, 
            marker=metric_markers[0], markersize=8, label='Time vs Tokens', alpha=0.8,
            linestyle=metric_line_styles[0])
    
    # Plot 2: Cost vs Tokens (use secondary y-axis)
    ax2 = ax.twinx()
    y_tokens_cost_sorted = np.array(data["cost"])[sorted_indices_tokens]
    ax2.plot(x_tokens_sorted, y_tokens_cost_sorted, color=metric_colors[1], linewidth=3, 
             marker=metric_markers[1], markersize=8, label='Cost vs Tokens', alpha=0.8,
             linestyle=metric_line_styles[1])
    
    # Plot 3: Time vs Messages (normalize messages to token scale for comparison)
    sorted_indices_messages = np.argsort(messages)
    x_messages_sorted = np.array(messages)[sorted_indices_messages]
    y_messages_sorted = np.array(data["time"])[sorted_indices_messages]
    # Normalize messages to token scale for visualization
    x_messages_norm = (x_messages_sorted - min(messages)) / (max(messages) - min(messages)) * (max(tokens) - min(tokens)) + min(tokens)
    ax.plot(x_messages_norm, y_messages_sorted, color=metric_colors[2], linewidth=3, 
            marker=metric_markers[2], markersize=8, label='Time vs Messages', alpha=0.8,
            linestyle=metric_line_styles[2])
    
    # Plot 4: Cost vs Messages
    y_messages_cost_sorted = np.array(data["cost"])[sorted_indices_messages]
    ax2.plot(x_messages_norm, y_messages_cost_sorted, color=metric_colors[3], linewidth=3, 
             marker=metric_markers[3], markersize=8, label='Cost vs Messages', alpha=0.8,
             linestyle=metric_line_styles[3])
    
    # Set labels and title
    ax.set_xlabel('Number of Tokens in Log', fontsize=12, fontweight='bold')
    ax.set_ylabel('Time (s)', fontsize=12, fontweight='bold', color=metric_colors[0])
    ax2.set_ylabel('Cost ($)', fontsize=12, fontweight='bold', color=metric_colors[1])
    ax.set_title(f'{model.upper()}: All Performance Metrics (Token-based)', fontsize=14, fontweight='bold')
    
    # Add legends
    lines1, labels1 = ax.get_legend_handles_labels()
    lines2, labels2 = ax2.get_legend_handles_labels()
    ax.legend(lines1 + lines2, labels1 + labels2, loc='upper left', fontsize=9, 
              frameon=True, fancybox=True, shadow=True)
    
    ax.grid(True, alpha=0.3)
    ax.set_xscale('log')
    ax.set_xlim(10000, 10000000)
    
    # Add performance summary
    avg_time = np.mean(data["time"])
    avg_cost = np.mean(data["cost"])
    ax.text(0.02, 0.98, f'Avg Time: {avg_time:.1f}s\nAvg Cost: ${avg_cost:.2f}', 
            transform=ax.transAxes, fontsize=10, verticalalignment='top',
            bbox=dict(boxstyle="round,pad=0.3", facecolor="white", alpha=0.8))

plt.tight_layout()
plt.show()

# Save the third plot
plot3_path = os.path.join(current_dir, 'llm_comprehensive_performance_metrics_tokens.png')
fig3.savefig(plot3_path, dpi=300, bbox_inches='tight', facecolor='white')
print(f"📊 Third plot saved to: {plot3_path}")

# Print elegant summary
print("=" * 70)
print("🎯 LLM PERFORMANCE ANALYSIS SUMMARY")
print("=" * 70)

for model, data in models.items():
    avg_time = np.mean(data["time"])
    avg_cost = np.mean(data["cost"])
    total_time = np.sum(data["time"])
    total_cost = np.sum(data["cost"])
    
    print(f"\n📊 {model.upper()}:")
    print(f"   ⏱️  Average Time: {avg_time:.1f}s")
    print(f"   💰 Average Cost: ${avg_cost:.2f}")
    print(f"   📈 Total Time: {total_time:.1f}s")
    print(f"   💸 Total Cost: ${total_cost:.2f}")

best_time_model = min(models.items(), key=lambda x: np.mean(x[1]['time']))[0]
best_cost_model = min(models.items(), key=lambda x: np.mean(x[1]['cost']))[0]

print(f"\n🏆 PERFORMANCE RANKINGS:")
print(f"   🥇 Fastest Model: {best_time_model}")
print(f"   🥇 Most Cost-Effective: {best_cost_model}")
print("=" * 70)
