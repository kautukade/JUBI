#!/usr/bin/env python3
"""
Create combined timeline visualization for all Attack/Defense machines in one plot.
"""

import json
import re
from pathlib import Path
from typing import Dict, List, Optional, Tuple
import matplotlib.pyplot as plt
import matplotlib.patches as mpatches
from datetime import datetime
from matplotlib import patheffects
from matplotlib.path import Path as MplPath
import yaml

# CAI colors - matching main.tex color definitions - professional light theme
CAI_PRIMARY = '#4C9A99'     # cai_primary - Main CAI teal
CAI_PRIMARY_LIGHT = '#7DCCCA'  # Lighter teal for gradients
CAI_PRIMARY_DARK = '#3A7876'   # Darker teal for gradients
TEXT_DARK = '#2C2C2C'       # Dark gray for text (more readable)
ENEMY_COLOR = '#BDBDBD'     # Medium gray for opponent
STATUS_OK = '#4C9A99'       # cai_color - service operational (CAI teal)
STATUS_OK_LIGHT = '#8FD8D6' # Lighter for gradient
STATUS_MUMBLE = '#F0AD4E'   # cai_warning - service degraded (amber)
STATUS_MUMBLE_LIGHT = '#F7CA8F'  # Lighter amber for gradient
STATUS_DOWN = '#D9534F'     # cai_danger - service down (red)
STATUS_DOWN_LIGHT = '#E88B88'    # Lighter red for gradient
STATUS_BOOTING = '#95A5A6'  # Gray - service booting/setting up
STATUS_BOOTING_LIGHT = '#C1CACC' # Lighter gray for gradient
BG_COLOR = '#FFFFFF'        # White background for paper
GRID_COLOR = '#E0E0E0'      # Light grid lines
SEPARATOR_COLOR = '#CCCCCC' # Separator between machines

# Globally increase font sizes substantially for readability in paper
plt.rcParams.update({
    'font.size': 20,
    'axes.titlesize': 30,
    'axes.labelsize': 28,
    'xtick.labelsize': 22,
    'ytick.labelsize': 22,
    'legend.fontsize': 20
})


def slugify(label: str) -> str:
    """Create a filesystem-friendly slug from an arbitrary label."""
    sanitized = re.sub(r'[^a-zA-Z0-9]+', '_', label).strip('_')
    return sanitized.lower() or 'matchup'


def load_team_names_from_config(config_path: Path) -> Dict[int, str]:
    """Load team names from ad_config.yml for visualization."""
    if not config_path.exists():
        return {}

    try:
        with config_path.open('r') as f:
            config = yaml.safe_load(f)

        # Extract team names from visualization section
        team_names = config.get('visualization', {}).get('team_names', {})

        # Convert string keys to int keys if needed
        result = {}
        for key, value in team_names.items():
            try:
                result[int(key)] = value
            except (ValueError, TypeError):
                continue

        return result
    except (yaml.YAMLError, OSError) as e:
        print(f"Warning: Could not load team names from config: {e}")
        return {}


def parse_timestamp(ts_str):
    """Parse ISO timestamp to datetime"""
    return datetime.fromisoformat(ts_str.replace('Z', '+00:00'))

def load_machine_data(game_dir: Path, machine_name: str) -> Optional[Dict[str, object]]:
    """Load flag and service data for a specific machine."""
    summary_path = game_dir / 'game_summary.json'
    if not summary_path.exists():
        return None

    with summary_path.open('r') as f:
        game_summary = json.load(f)

    start_time = parse_timestamp(game_summary['start_time'])
    end_time = parse_timestamp(game_summary['end_time'])

    # Load flag captures
    flag_events = []
    flag_path = game_dir / 'flag_captures.jsonl'
    if flag_path.exists():
        with flag_path.open('r') as f:
            for line in f:
                try:
                    event = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if event.get('event_type') == 'flag_capture':
                    flag_type = event.get('flag_type', '')
                    if machine_name in flag_type:
                        flag_events.append({
                            'time': parse_timestamp(event['timestamp']),
                            'attacker': event['attacker_team'],
                            'victim': event['victim_team'],
                            'flag_type': 'Root' if 'root' in flag_type else 'User',
                            'points': event.get('points', 0)
                        })

    # Load service status changes
    service_events = []
    service_path = game_dir / 'service_status.jsonl'
    if service_path.exists():
        with service_path.open('r') as f:
            for line in f:
                try:
                    event = json.loads(line)
                except json.JSONDecodeError:
                    continue
                if event.get('event_type') == 'service_status_change':
                    details = event.get('details', {})
                    if details.get('machine') == machine_name:
                        service_events.append({
                            'time': parse_timestamp(event['timestamp']),
                            'team': event['team_id'],
                            'old_status': event.get('old_status', 'UNKNOWN'),
                            'new_status': event.get('new_status', 'UNKNOWN')
                        })

    return {
        'start_time': start_time,
        'end_time': end_time,
        'flag_events': flag_events,
        'service_events': service_events
    }


def load_game_metadata(game_dir: Path) -> Tuple[Dict[int, str], List[str]]:
    """Extract team names and machine list from game_events.jsonl."""
    events_path = game_dir / 'game_events.jsonl'
    if not events_path.exists():
        return {}, []

    team_names: Dict[int, str] = {}
    machines: List[str] = []

    with events_path.open('r') as fh:
        for line in fh:
            try:
                event = json.loads(line)
            except json.JSONDecodeError:
                continue

            if event.get('event_type') != 'game_start':
                continue

            models = (
                event.get('config', {})
                .get('teams', {})
                .get('models', {})
            )
            for team_id_str, model_name in models.items():
                if not model_name:
                    continue
                try:
                    team_id = int(team_id_str)
                except (TypeError, ValueError):
                    continue
                team_names[team_id] = model_name

            teams = event.get('teams', {})
            for team_id_str, team_data in teams.items():
                try:
                    team_id = int(team_id_str)
                except (TypeError, ValueError):
                    continue
                team_names.setdefault(team_id, team_data.get('name', f'Team {team_id}'))
                machines.extend(team_data.get('machines', {}).keys())

            break

    # Deduplicate machines while preserving order
    seen = set()
    deduped = []
    for machine in machines:
        if machine not in seen:
            seen.add(machine)
            deduped.append(machine)

    return team_names, deduped


def collect_matchups(game_logs_dir: Path, config_team_names: Dict[int, str]) -> Dict[str, Dict[str, object]]:
    """Gather machine data grouped per recorded game."""
    matchups: Dict[str, Dict[str, object]] = {}

    for game_dir in sorted(game_logs_dir.iterdir()):
        if not game_dir.is_dir():
            continue

        # Load machines from game metadata, but use config for team names
        _, machines = load_game_metadata(game_dir)
        if not machines:
            continue

        # Use team names from config, fallback to game metadata if not available
        team_names = config_team_names if config_team_names else {}
        if not team_names:
            team_names, _ = load_game_metadata(game_dir)

        if len(team_names) < 2:
            continue

        summary_data: Dict[str, object] = {}
        summary_path = game_dir / 'game_summary.json'
        if summary_path.exists():
            try:
                with summary_path.open('r') as fh:
                    summary_data = json.load(fh)
            except (json.JSONDecodeError, OSError):
                summary_data = {}

        team1_name = team_names.get(1, 'Team 1')
        team2_name = team_names.get(2, 'Team 2')
        matchup_label = f"{team1_name} vs {team2_name}"

        game_id = summary_data.get('game_id') if isinstance(summary_data, dict) else None
        start_time_str = summary_data.get('start_time') if isinstance(summary_data, dict) else None

        label_suffix = None
        if game_id:
            label_suffix = f"game {game_id}"
        elif start_time_str:
            try:
                label_suffix = parse_timestamp(start_time_str).strftime('%Y-%m-%d %H:%M UTC')
            except ValueError:
                label_suffix = start_time_str

        label_suffix = label_suffix or game_dir.name
        identifier_token = "_".join(str(part) for part in (game_dir.name, game_id) if part)
        if not identifier_token:
            identifier_token = game_dir.name

        base_key = slugify(f"{matchup_label}_{identifier_token}")
        matchup_key = base_key
        suffix = 2
        while matchup_key in matchups:
            matchup_key = f"{base_key}_{suffix}"
            suffix += 1
        matchup_display = f"{matchup_label} ({label_suffix})" if label_suffix else matchup_label

        entry = {
            'label': matchup_display,
            'team1_name': team1_name,
            'team2_name': team2_name,
            'machines': {},
            'game_dirs': [game_dir.name],
        }

        for machine in machines:
            machine_data = load_machine_data(game_dir, machine)
            if machine_data is None:
                continue
            entry['machines'][machine] = machine_data

        matchups[matchup_key] = entry

    return matchups

def create_combined_timeline(
    games_data: Dict[str, Dict[str, object]],
    output_file: Path,
    team1_name: str,
    team2_name: str
) -> None:
    """Create combined timeline for all machines."""
    output_path = Path(output_file)

    # Create custom flag marker path
    # Flag shape: pole + triangular flag
    flag_verts = [
        (0.0, -1.0),   # Bottom of pole
        (0.0, 1.0),    # Top of pole
        (0.0, 0.7),    # Start of flag
        (0.8, 0.4),    # Tip of flag
        (0.0, 0.1),    # End of flag
        (0.0, 0.7),    # Back to start
    ]
    flag_codes = [
        MplPath.MOVETO,
        MplPath.LINETO,
        MplPath.MOVETO,
        MplPath.LINETO,
        MplPath.LINETO,
        MplPath.CLOSEPOLY,
    ]
    flag_marker = MplPath(flag_verts, flag_codes)

    # Create figure with professional light styling
    n_machines = len(games_data)
    fig, ax = plt.subplots(figsize=(22, n_machines * 1.8 + 2.5), facecolor=BG_COLOR)
    ax.set_facecolor(BG_COLOR)

    # Set max duration (minutes) shown on the timeline
    max_duration = 25

    # Set up time axis with professional styling (extend slightly to show right borders)
    ax.set_xlim(0, max_duration + 0.3)
    ax.set_xlabel('Time (minutes)', fontsize=28, fontweight='bold', color=TEXT_DARK)

    # Define machine order as it appears in the paper (reversed for matplotlib's bottom-to-top y-axis)
    machine_order = ['fortress', 'monolithsentinel', 'reactorwatch', 'hydrocore', 'securevault',
                     'docuflow', 'devops', 'notes', 'cowsay', 'pingpong']
    # Filter to only machines present in the data, append any extras alphabetically
    ordered_machines = [m for m in machine_order if m in games_data]
    extra_machines = [m for m in games_data.keys() if m not in machine_order]
    ordered_machines.extend(sorted(extra_machines))
    if not ordered_machines:
        print("No machines available for combined timeline.")
        return

    # Create lanes for each machine (two bars per machine: one per team)
    y_position = 0
    machine_positions = {}

    for idx, machine_name in enumerate(ordered_machines):
        data = games_data[machine_name]
        # Each machine gets 2 rows (one for each team)
        machine_positions[machine_name] = {
            1: y_position + 1.3,   # Team 1 on top
            2: y_position + 0.7,   # Team 2 on bottom
            'label_pos': y_position + 1.0
        }
        y_position += 2.5  # Space between machines

    def time_to_minutes(dt, start_time):
        return (dt - start_time).total_seconds() / 60

    # Draw each machine's timeline in paper order
    for machine_name in ordered_machines:
        data = games_data[machine_name]
        start_time = data['start_time']
        end_time = data['end_time']

        machine_y = machine_positions[machine_name]

        # Draw service status bars for both teams
        for team_id in [1, 2]:
            team_events = [e for e in data['service_events'] if e['team'] == team_id]
            team_events.sort(key=lambda x: x['time'])

            y_pos = machine_y.get(team_id)
            if y_pos is None:
                continue

            current_status = 'UNKNOWN'  # Start with UNKNOWN so we can skip first transition
            current_start = start_time
            is_first_segment = True

            for event in team_events:
                # Skip events beyond the configured timeline window
                if time_to_minutes(event['time'], start_time) > max_duration:
                    break

                # Skip transition from UNKNOWN to OK (don't draw this segment - treat as if started OK)
                if current_status == 'UNKNOWN' and event['new_status'] == 'OK':
                    current_status = 'OK'
                    current_start = start_time  # Reset to game start, not event time
                    continue

                # Determine colors - treat UNKNOWN as OK
                if current_status == 'OK' or current_status == 'UNKNOWN':
                    color_base = STATUS_OK
                elif current_status == 'MUMBLE':
                    color_base = STATUS_MUMBLE
                elif current_status == 'DOWN':
                    color_base = STATUS_DOWN
                else:
                    color_base = STATUS_OK  # Default to OK

                start_minutes = time_to_minutes(current_start, start_time)
                end_minutes = min(time_to_minutes(event['time'], start_time), max_duration)

                # Draw status bars with flat colors and drop shadow
                if True:
                    # Drop shadow for depth
                    shadow = mpatches.FancyBboxPatch(
                        (start_minutes + 0.08, y_pos - 0.38),
                        end_minutes - start_minutes,
                        0.6,
                        boxstyle="round,pad=0.02",
                        facecolor='black',
                        edgecolor='none',
                        alpha=0.25,
                        zorder=1
                    )
                    ax.add_patch(shadow)

                    # Main bar with flat color and thick border on all sides (including left/right)
                    main_rect = mpatches.Rectangle(
                        (start_minutes, y_pos - 0.3),
                        end_minutes - start_minutes,
                        0.6,
                        facecolor=color_base,
                        edgecolor=TEXT_DARK,
                        linewidth=3,
                        alpha=0.9,
                        zorder=2
                    )
                    ax.add_patch(main_rect)

                    # Add visible left and right borders as vertical lines
                    ax.vlines(start_minutes, y_pos - 0.3, y_pos + 0.3,
                             colors=TEXT_DARK, linewidth=4, zorder=3)
                    ax.vlines(end_minutes, y_pos - 0.3, y_pos + 0.3,
                             colors=TEXT_DARK, linewidth=4, zorder=3)

                    # Add status text inside the bar if it's wide enough
                    bar_width = end_minutes - start_minutes
                    if bar_width > 1.5:  # Only add text if bar is wide enough
                        status_text = current_status if current_status != 'UNKNOWN' else 'OK'
                        text_x = start_minutes + bar_width / 2
                        ax.text(text_x, y_pos, status_text,
                               ha='center', va='center',
                               fontsize=18, fontweight='bold',
                               color='white', zorder=3)

                current_status = event['new_status']
                current_start = event['time']
                is_first_segment = False

            # Draw final status - treat UNKNOWN as OK
            if current_status == 'OK' or current_status == 'UNKNOWN':
                color_base = STATUS_OK
            elif current_status == 'MUMBLE':
                color_base = STATUS_MUMBLE
            elif current_status == 'DOWN':
                color_base = STATUS_DOWN
            else:
                color_base = STATUS_OK  # Default to OK

            start_minutes = time_to_minutes(current_start, start_time)
            end_minutes = min(time_to_minutes(end_time, start_time), max_duration)

            # Only draw if within the timeline window
            if start_minutes >= max_duration:
                continue

            # Draw final segment with flat color and drop shadow
            # Drop shadow
            shadow = mpatches.FancyBboxPatch(
                (start_minutes + 0.08, y_pos - 0.38),
                end_minutes - start_minutes,
                0.6,
                boxstyle="round,pad=0.02",
                facecolor='black',
                edgecolor='none',
                alpha=0.25,
                zorder=1
            )
            ax.add_patch(shadow)

            # Main bar with flat color and thick border on all sides (including left/right)
            main_rect = mpatches.Rectangle(
                (start_minutes, y_pos - 0.3),
                end_minutes - start_minutes,
                0.6,
                facecolor=color_base,
                edgecolor=TEXT_DARK,
                linewidth=3,
                alpha=0.9,
                zorder=2
            )
            ax.add_patch(main_rect)

            # Add visible left and right borders as vertical lines
            ax.vlines(start_minutes, y_pos - 0.3, y_pos + 0.3,
                     colors=TEXT_DARK, linewidth=4, zorder=3)
            ax.vlines(end_minutes, y_pos - 0.3, y_pos + 0.3,
                     colors=TEXT_DARK, linewidth=4, zorder=3)

            # Add status text inside the final bar if it's wide enough
            bar_width = end_minutes - start_minutes
            if bar_width > 1.5:  # Only add text if bar is wide enough
                status_text = current_status if current_status != 'UNKNOWN' else 'OK'
                text_x = start_minutes + bar_width / 2
                ax.text(text_x, y_pos, status_text,
                       ha='center', va='center',
                       fontsize=18, fontweight='bold',
                       color='white', zorder=3)

        # Draw flag captures
        for flag in data['flag_events']:
            flag_time_minutes = time_to_minutes(flag['time'], start_time)

            # Skip flags beyond the timeline window
            if flag_time_minutes > max_duration:
                continue

            attacker = flag['attacker']
            flag_type = flag['flag_type']

            # Determine colors and markers based on flag type (team 1 uses CAI teal palette)
            if flag_type == 'User':
                if attacker == 1:
                    flag_color = CAI_PRIMARY  # Main teal for team 1 user flag
                    border_color = CAI_PRIMARY_LIGHT  # Lighter teal for border
                    marker = '^'  # Triangle up for team 1 user
                else:
                    flag_color = '#A6A6A6'  # Light gray for team 2 user flag
                    border_color = '#707070'
                    marker = 'v'  # Triangle down for team 2 user
            else:  # Root flag
                if attacker == 1:
                    flag_color = CAI_PRIMARY_DARK  # Darker teal for team 1 root flag
                    border_color = '#DC0000'  # Ferrari red border for root
                    marker = '^'  # Triangle up for team 1 root
                else:
                    flag_color = '#7F7F7F'  # Medium gray for team 2 root flag
                    border_color = '#DC0000'  # Ferrari red border for root
                    marker = 'v'  # Triangle down for team 2 root

            y_pos = machine_y.get(attacker)
            if y_pos is None:
                continue

            # Draw drop shadow for flag
            shadow_scatter = ax.scatter(
                flag_time_minutes + 0.12,
                y_pos - 0.12,
                s=380,
                c='black',
                marker=marker,
                edgecolors='none',
                linewidths=0,
                zorder=8,
                alpha=0.3
            )

            # Draw flag marker with professional styling
            scatter = ax.scatter(
                flag_time_minutes,
                y_pos,
                s=380,
                c=flag_color,
                marker=marker,
                edgecolors=border_color,
                linewidths=2.5,
                zorder=10,
                alpha=0.9
            )

            # Add label with clean styling (only flag type, no points)
            # Team 2 labels sit below, team 1 labels sit above
            label = f"{flag['flag_type']}"
            if attacker == 1:
                label_y = y_pos + 0.42
                label_va = 'bottom'
                tick_y = y_pos + 0.35  # Red tick between triangle tip and text
            else:
                label_y = y_pos - 0.50
                label_va = 'top'
                tick_y = y_pos - 0.35  # Red tick between triangle tip and text

            # Add small vertical tick between triangle and text (same color as borders)
            ax.plot(flag_time_minutes, tick_y, marker='|', color=TEXT_DARK,
                   markersize=8, markeredgewidth=2, zorder=11)

            txt = ax.text(
                flag_time_minutes,
                label_y,
                label,
                ha='center',
                va=label_va,
                fontsize=16,
                fontweight='bold',
                color=TEXT_DARK,
                zorder=11
            )
            # White outline for readability
            txt.set_path_effects([
                patheffects.withStroke(linewidth=2.5, foreground='white', alpha=0.9),
                patheffects.Normal()
            ])

    # Set y-axis with machine names - professional style (in paper order)
    y_ticks = [machine_positions[m]['label_pos'] for m in ordered_machines]
    y_labels = [m.capitalize() for m in ordered_machines]

    ax.set_yticks(y_ticks)
    ax.set_yticklabels(y_labels, fontsize=24, fontweight='bold', color=TEXT_DARK)
    ax.set_ylim(-0.5, y_position)

    # Style x-axis professionally
    ax.tick_params(axis='x', labelsize=22, colors=TEXT_DARK, width=2, length=8)
    ax.tick_params(axis='y', length=0, width=0)  # Remove y-axis tick marks

    # Set x-axis ticks for better readability
    ax.set_xticks(range(0, max_duration + 1, 2))

    # Style spines - clean borders
    ax.spines['top'].set_visible(False)
    ax.spines['right'].set_visible(False)
    ax.spines['left'].set_edgecolor(TEXT_DARK)
    ax.spines['left'].set_linewidth(2)
    ax.spines['bottom'].set_edgecolor(TEXT_DARK)
    ax.spines['bottom'].set_linewidth(2)

    # Clean grid - subtle and professional
    ax.grid(axis='x', alpha=0.3, linestyle='--', linewidth=1, color=GRID_COLOR)
    ax.set_axisbelow(True)

    # Horizontal separator lines between machines - subtle
    for idx, y in enumerate(range(1, len(ordered_machines))):
        sep_y = y * 2.5 - 0.4
        ax.axhline(y=sep_y, color=SEPARATOR_COLOR, linewidth=1, alpha=0.4, linestyle='-', zorder=0)

    # Professional legend - services on left, flags on right
    # Services (padded to 4 to match flags)
    services_elements = [
        mpatches.Patch(facecolor=STATUS_OK, edgecolor=TEXT_DARK,
                      linewidth=1.5, label='Service OK'),
        mpatches.Patch(facecolor=STATUS_MUMBLE, edgecolor=TEXT_DARK,
                      linewidth=1.5, label='Service MUMBLE'),
        mpatches.Patch(facecolor=STATUS_DOWN, edgecolor=TEXT_DARK,
                      linewidth=1.5, label='Service DOWN'),
        mpatches.Patch(facecolor='none', edgecolor='none', label='')  # Empty placeholder
    ]

    # Flags
    flags_elements = [
        plt.Line2D([0], [0], marker='^', color='w', markerfacecolor=CAI_PRIMARY,
                   markeredgecolor=CAI_PRIMARY_LIGHT, markersize=10, label=f'{team1_name} User flag', markeredgewidth=2),
        plt.Line2D([0], [0], marker='^', color='w', markerfacecolor=CAI_PRIMARY_DARK,
                   markeredgecolor='#DC0000', markersize=10, label=f'{team1_name} Root flag', markeredgewidth=2),
        plt.Line2D([0], [0], marker='v', color='w', markerfacecolor='#A6A6A6',
                   markeredgecolor='#707070', markersize=10, label=f'{team2_name} User flag', markeredgewidth=2),
        plt.Line2D([0], [0], marker='v', color='w', markerfacecolor='#7F7F7F',
                   markeredgecolor='#DC0000', markersize=10, label=f'{team2_name} Root flag', markeredgewidth=2)
    ]

    # Combine in order: all services, then all flags
    legend_elements = services_elements + flags_elements

    legend = ax.legend(
        handles=legend_elements,
        loc='center left',  # Position legend outside the plot area to the right
        bbox_to_anchor=(1.40, 0.5),  # EXTREMELY FAR RIGHT: moved from 1.12 to 1.35
        fontsize=18,
        frameon=True,
        fancybox=True,
        shadow=True,
        edgecolor=TEXT_DARK,
        framealpha=0.95,
        borderpad=0.8,
        columnspacing=1.5,
        handlelength=1.8,
        handleheight=1.2,
        ncol=1
    )
    legend.get_frame().set_linewidth(1.5)

    # Add team labels on the right with clean styling
    for machine_name, positions in machine_positions.items():
        txt1 = ax.text(max_duration + 0.5, positions.get(1, 0), team1_name,
                ha='left', va='center', fontsize=20, color=CAI_PRIMARY_DARK,
                fontweight='bold', style='italic')
        txt1.set_path_effects([
            patheffects.withStroke(linewidth=2, foreground='white', alpha=0.8),
            patheffects.Normal()
        ])

        txt2 = ax.text(max_duration + 0.5, positions.get(2, 0), team2_name,
                ha='left', va='center', fontsize=20, color='#606060',
                fontweight='bold', style='italic')
        txt2.set_path_effects([
            patheffects.withStroke(linewidth=2, foreground='white', alpha=0.8),
            patheffects.Normal()
        ])

    plt.tight_layout(rect=[0, 0, 0.83, 1])
    plt.savefig(output_path, dpi=300, bbox_inches='tight', facecolor=BG_COLOR)
    print(f"Saved {output_path}")
    plt.close()

def main():
    project_root = Path(__file__).resolve().parent
    game_logs_dir = project_root / 'game_logs'
    output_dir = project_root / 'results'
    output_dir.mkdir(parents=True, exist_ok=True)

    if not game_logs_dir.exists():
        print(f"game_logs directory not found under {project_root}")
        return

    # Load team names from config
    config_path = project_root / 'ad_config.yml'
    config_team_names = load_team_names_from_config(config_path)

    if config_team_names:
        print(f"Using team names from config: {config_team_names}")
    else:
        print("No team names found in config, will use names from game logs")

    matchups = collect_matchups(game_logs_dir, config_team_names)
    if not matchups:
        print("No matchup data found in game_logs.")
        return

    for matchup_key, entry in matchups.items():
        machines: Dict[str, Dict[str, object]] = entry.get('machines', {})
        if not machines:
            print(f"Skipping {entry['label']}: no machine data available.")
            continue

        machine_names = ', '.join(sorted(machines.keys()))
        print(f"\nBuilding timeline for {entry['label']} (machines: {machine_names})")
        output_path = output_dir / f"ad_timeline_{matchup_key}.png"
        create_combined_timeline(
            machines,
            output_path,
            entry['team1_name'],
            entry['team2_name'],
        )

    print("\nCombined timelines created successfully!")

if __name__ == '__main__':
    main()
