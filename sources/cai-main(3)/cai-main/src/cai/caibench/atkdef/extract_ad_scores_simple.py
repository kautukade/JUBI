#!/usr/bin/env python3
"""
Extract Attack/Defense CTF scores and create simple horizontal bar visualizations.
"""

import json
import re
from datetime import datetime
from pathlib import Path
from typing import Dict, List, Optional, Tuple
import matplotlib.pyplot as plt
import numpy as np
from matplotlib import patheffects
import yaml

# CAI colors from the LaTeX document
CAI_PRIMARY = '#4C9A99'  # Main CAI color - lighter teal
TIE_COLOR = '#335757'    # Dark teal for ties
ENEMY_COLOR = '#D6D6D6'  # Lighter gray for opponent
RED_LINE = '#D9534F'     # Softer red for central line (cai_danger from main.tex)
TEXT_DARK = '#335757'    # Dark teal for text on light bars


def slugify(label: str) -> str:
    """Create a filesystem-friendly slug from an arbitrary label."""
    sanitized = re.sub(r'[^a-zA-Z0-9]+', '_', label).strip('_')
    if not sanitized:
        sanitized = 'matchup'
    return sanitized.lower()


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


def load_team_metadata(game_dir: Path) -> Tuple[Dict[int, str], Optional[str]]:
    """Extract team display names and game_id from the game_events.jsonl file."""
    events_path = game_dir / 'game_events.jsonl'
    if not events_path.exists():
        return {}, None

    team_names: Dict[int, str] = {}
    game_id: Optional[str] = None

    with events_path.open() as fh:
        for line in fh:
            try:
                event = json.loads(line)
            except json.JSONDecodeError:
                continue

            if event.get('event_type') != 'game_start':
                continue

            game_id = event.get('game_id')

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
            for team_id_str, team_info in teams.items():
                try:
                    team_id = int(team_id_str)
                except (TypeError, ValueError):
                    continue
                team_names.setdefault(team_id, team_info.get('name', f'Team {team_id}'))

            break

    return team_names, game_id

def extract_final_scores(score_file):
    """Extract final score breakdown per team from score_changes.jsonl.

    Some recent logs no longer emit explicit ``score_breakdown`` events and
    only contain incremental ``score_change`` entries. When that happens we
    reconstruct the same data structure by aggregating the per-machine deltas
    encoded in the ``reason`` field (e.g. ``defense_points_cowsay`` or
    ``flag_capture_cowsay_user_flag``).
    """

    final_breakdowns: Dict[int, Dict[str, object]] = {}
    fallback_totals: Dict[int, Dict[str, object]] = {}

    def infer_machine_from_reason(reason: str) -> Optional[str]:
        match = re.search(r'(?:flag_capture|defense_points|service_down_penalty|sla_penalty|penalty_points)_(?P<machine>[^_]+)', reason)
        if match:
            return match.group('machine')
        return None

    with open(score_file, 'r') as f:
        for line in f:
            try:
                data = json.loads(line)
            except json.JSONDecodeError:
                continue

            event_type = data.get('event_type')
            team_id = data.get('team_id')
            if team_id is None:
                continue

            try:
                team_id = int(team_id)
            except (TypeError, ValueError):
                continue

            if event_type == 'score_breakdown':
                final_breakdowns[team_id] = data
                continue

            if event_type != 'score_change':
                continue

            change = int(data.get('change', 0) or 0)
            reason = data.get('reason', '') or ''
            new_score = int(data.get('new_score', 0) or 0)

            entry = fallback_totals.setdefault(team_id, {
                'attack_points': 0,
                'defense_points': 0,
                'penalty_points': 0,
                'unclassified_points': 0,
                'machine_breakdown': {},
                'total_score': 0,
            })

            entry['total_score'] = max(entry['total_score'], new_score)

            category: Optional[str] = None
            if 'flag_capture' in reason:
                entry['attack_points'] += change
                category = 'attack_points'
            elif 'defense_points' in reason:
                entry['defense_points'] += change
                category = 'defense_points'
            elif 'penalty' in reason:
                entry['penalty_points'] += change
                category = 'penalty_points'
            else:
                entry['unclassified_points'] += change

            machine = infer_machine_from_reason(reason)
            if machine:
                machine_entry = entry['machine_breakdown'].setdefault(
                    machine,
                    {'attack_points': 0, 'defense_points': 0, 'penalty_points': 0, 'total': 0},
                )
                if category:
                    machine_entry[category] += change
                machine_entry['total'] = (
                    machine_entry['attack_points']
                    + machine_entry['defense_points']
                    + machine_entry['penalty_points']
                )

    if final_breakdowns:
        return final_breakdowns

    reconstructed: Dict[int, Dict[str, object]] = {}
    for team_id, data in fallback_totals.items():
        total_score = data['attack_points'] + data['defense_points'] + data['penalty_points'] + data['unclassified_points']
        if not total_score:
            total_score = data['total_score']

        reconstructed[team_id] = {
            'event_type': 'score_breakdown',
            'team_id': team_id,
            'total_score': total_score,
            'breakdown': {
                'attack_points': data['attack_points'],
                'defense_points': data['defense_points'],
                'penalty_points': data['penalty_points'],
                'unclassified_points': data['unclassified_points'],
            },
            'machine_breakdown': {
                machine: {
                    'attack_points': machine_data['attack_points'],
                    'defense_points': machine_data['defense_points'],
                    'penalty_points': machine_data['penalty_points'],
                    'total': machine_data['total'],
                }
                for machine, machine_data in data['machine_breakdown'].items()
            },
        }

    return reconstructed

def parse_game_logs(base_dir: Path, config_team_names: Dict[int, str]) -> Dict[str, Dict[str, object]]:
    """Parse every game directory and collect per-machine final scores."""
    base_path = Path(base_dir)
    if not base_path.exists():
        raise FileNotFoundError(f"Game logs directory not found: {base_path}")

    results: Dict[str, Dict[str, object]] = {}

    for game_dir in sorted(base_path.iterdir()):
        if not game_dir.is_dir():
            continue

        score_file = game_dir / 'score_changes.jsonl'
        if not score_file.exists():
            continue

        breakdowns = extract_final_scores(score_file)
        if not breakdowns:
            continue

        summary_data: Dict[str, object] = {}
        summary_path = game_dir / 'game_summary.json'
        if summary_path.exists():
            try:
                with summary_path.open('r') as fh:
                    summary_data = json.load(fh)
            except (json.JSONDecodeError, OSError):
                summary_data = {}

        # Use team names from config, fallback to game metadata if not available
        if config_team_names:
            team_names = config_team_names
        else:
            team_names, _ = load_team_metadata(game_dir)

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
                label_suffix = datetime.fromisoformat(start_time_str.replace('Z', '+00:00')).strftime('%Y-%m-%d %H:%M UTC')
            except ValueError:
                label_suffix = start_time_str

        label_suffix = label_suffix or game_dir.name
        identifier_token = "_".join(str(part) for part in (game_dir.name, game_id) if part)
        if not identifier_token:
            identifier_token = game_dir.name

        base_key = slugify(f"{matchup_label}_{identifier_token}")
        matchup_key = base_key
        suffix = 2
        while matchup_key in results:
            matchup_key = f"{base_key}_{suffix}"
            suffix += 1
        matchup_entry = {
            'label': f"{matchup_label} ({label_suffix})" if label_suffix else matchup_label,
            'team1_name': team1_name,
            'team2_name': team2_name,
            'game_dirs': [game_dir.name],
            'machines': {},
        }

        for team_id, breakdown in breakdowns.items():
            machine_breakdown = breakdown.get('machine_breakdown', {})
            for machine, scores in machine_breakdown.items():
                machine_entry = matchup_entry['machines'].setdefault(
                    machine,
                    {'team1': 0, 'team2': 0},
                )
                total = int(scores.get('total', 0) or 0)
                if team_id == 1:
                    machine_entry['team1'] = total
                elif team_id == 2:
                    machine_entry['team2'] = total

        results[matchup_key] = matchup_entry

    return results

def create_score_visualization(results, matchup, team1_name, team2_name, output_file):
    """Create horizontal bar chart showing Win/Tie/Lose with scores"""

    # Order as in main.tex Attack/Defense table (reversed for bottom-to-top display)
    machine_order = ['fortress', 'monolithsentinel', 'reactorwatch', 'hydrocore',
                     'securevault', 'docuflow', 'devops', 'notes', 'cowsay', 'pingpong']

    # Filter to only machines present in results and maintain order
    machines = [m for m in machine_order if m in results]
    n_machines = len(machines)

    if n_machines == 0:
        print(f"No data for {matchup}")
        return

    # Calculate percentages for each machine
    data = []
    for machine in machines:
        team1_score = results[machine]['team1']
        team2_score = results[machine]['team2']
        total_score = team1_score + team2_score

        # For ties, show 50-50 split with both colors
        if team1_score == team2_score:
            win_pct = 50
            lose_pct = 50
            is_tie = True
        elif total_score == 0:
            # Both scored 0
            win_pct = 50
            lose_pct = 50
            is_tie = True
        else:
            # Normal case - calculate percentages
            win_pct = (team1_score / total_score) * 100
            lose_pct = (team2_score / total_score) * 100
            is_tie = False

        data.append({
            'machine': machine,
            'team1_score': team1_score,
            'team2_score': team2_score,
            'win_pct': win_pct,
            'lose_pct': lose_pct,
            'is_tie': is_tie
        })

    # Create figure
    fig, ax = plt.subplots(figsize=(14, n_machines * 0.8))

    y_pos = np.arange(len(data))

    # Plot bars with fancy effects
    for i, d in enumerate(data):
        # Team 1 portion (always on the left)
        if d['win_pct'] > 0:
            bars1 = ax.barh(i, d['win_pct'], color=CAI_PRIMARY, height=0.75)
            # Add subtle shadow effect
            for bar in bars1:
                bar.set_path_effects([patheffects.SimplePatchShadow(offset=(2, -2), shadow_rgbFace='black', alpha=0.3),
                                     patheffects.Normal()])
            # Add score label with white text and dark border for contrast
            if d['win_pct'] > 8:
                txt = ax.text(d['win_pct']/2, i, f"{d['team1_score']}",
                       ha='center', va='center', color='white', fontweight='bold', fontsize=13)
                # Add dark border/stroke for readability
                txt.set_path_effects([
                    patheffects.withStroke(linewidth=3, foreground=TEXT_DARK, alpha=0.8),
                    patheffects.Normal()
                ])

        # Opponent portion (always on the right)
        if d['lose_pct'] > 0:
            bars2 = ax.barh(i, d['lose_pct'], left=d['win_pct'], color=ENEMY_COLOR, height=0.75)
            # Add subtle shadow effect
            for bar in bars2:
                bar.set_path_effects([patheffects.SimplePatchShadow(offset=(2, -2), shadow_rgbFace='black', alpha=0.3),
                                     patheffects.Normal()])
            # Add score label with dark text and white border for light bars
            if d['lose_pct'] > 8:
                txt = ax.text(d['win_pct'] + d['lose_pct']/2, i, f"{d['team2_score']}",
                       ha='center', va='center', color=TEXT_DARK, fontweight='bold', fontsize=13)
                # Add white border/stroke for contrast on light background
                txt.set_path_effects([
                    patheffects.withStroke(linewidth=3, foreground='white', alpha=0.8),
                    patheffects.Normal()
                ])

    # Set labels with bold font for machine names
    ax.set_yticks(y_pos)
    ax.set_yticklabels([d['machine'].capitalize() for d in data], fontsize=13, fontweight='bold')
    ax.set_xlim(0, 100)
    ax.set_xlabel('Score Distribution', fontsize=14, fontweight='bold')

    # Hide x-axis tick labels (numbers)
    ax.set_xticklabels([])

    # Add central vertical line at 50% in CAI color (dashed for subtlety)
    ax.axvline(x=50, color=CAI_PRIMARY, linewidth=1.5, linestyle='--', alpha=0.6, zorder=0)

    # Legend with bigger font and fancy styling (outside the plot area)
    from matplotlib.patches import Patch
    legend_elements = [
        Patch(facecolor=CAI_PRIMARY, label=team1_name),
        Patch(facecolor=ENEMY_COLOR, label=team2_name)
    ]
    ax.legend(handles=legend_elements, loc='upper left', bbox_to_anchor=(1.01, 1), fontsize=12, frameon=True, fancybox=True)

    # Grid
    ax.grid(axis='x', alpha=0.3)
    ax.set_axisbelow(True)

    plt.tight_layout()
    plt.savefig(output_file, dpi=300, bbox_inches='tight')
    print(f"Saved {output_file}")
    plt.close()

def create_overall_summary(results, matchup, team1_name, team2_name, output_file):
    """Create Win/Tie/Lose summary chart"""

    wins = 0
    ties = 0
    loses = 0

    for machine, scores in results.items():
        team1_total = scores['team1']
        team2_total = scores['team2']

        if team1_total > team2_total:
            wins += 1
        elif team1_total < team2_total:
            loses += 1
        else:
            ties += 1

    total = wins + ties + loses

    if total == 0:
        print(f"No data for {matchup}")
        return

    win_pct = (wins / total * 100)
    tie_pct = (ties / total * 100)
    lose_pct = (loses / total * 100)

    # Create figure
    fig, ax = plt.subplots(figsize=(14, 2))

    y_pos = 0

    # Win portion (team 1)
    if win_pct > 0:
        bars1 = ax.barh(y_pos, win_pct, color=CAI_PRIMARY, height=0.75)
        # Add subtle shadow effect
        for bar in bars1:
            bar.set_path_effects([patheffects.SimplePatchShadow(offset=(1, -1), shadow_rgbFace='black', alpha=0.2),
                                 patheffects.Normal()])
        if win_pct > 8:
            txt = ax.text(win_pct/2, y_pos, f'{win_pct:.1f}%',
                   ha='center', va='center', color='white', fontweight='bold', fontsize=13)
            txt.set_path_effects([
                patheffects.withStroke(linewidth=3, foreground=TEXT_DARK, alpha=0.8),
                patheffects.Normal()
            ])

    # Tie portion (black)
    if tie_pct > 0:
        bars2 = ax.barh(y_pos, tie_pct, left=win_pct, color=TIE_COLOR, height=0.75)
        # Add subtle shadow effect
        for bar in bars2:
            bar.set_path_effects([patheffects.SimplePatchShadow(offset=(1, -1), shadow_rgbFace='black', alpha=0.2),
                                 patheffects.Normal()])
        if tie_pct > 8:
            txt = ax.text(win_pct + tie_pct/2, y_pos, f'{tie_pct:.1f}%',
                   ha='center', va='center', color='white', fontweight='bold', fontsize=13)
            txt.set_path_effects([
                patheffects.withStroke(linewidth=3, foreground=TEXT_DARK, alpha=0.8),
                patheffects.Normal()
            ])

    # Lose portion (opponent)
    if lose_pct > 0:
        bars3 = ax.barh(y_pos, lose_pct, left=win_pct+tie_pct, color=ENEMY_COLOR, height=0.75)
        # Add subtle shadow effect
        for bar in bars3:
            bar.set_path_effects([patheffects.SimplePatchShadow(offset=(1, -1), shadow_rgbFace='black', alpha=0.2),
                                 patheffects.Normal()])
        if lose_pct > 8:
            txt = ax.text(win_pct + tie_pct + lose_pct/2, y_pos, f'{lose_pct:.1f}%',
                   ha='center', va='center', color=TEXT_DARK, fontweight='bold', fontsize=13)
            txt.set_path_effects([
                patheffects.withStroke(linewidth=3, foreground='white', alpha=0.8),
                patheffects.Normal()
            ])

    ax.set_xlim(0, 100)
    ax.set_ylim(-0.5, 0.5)
    ax.set_yticks([])
    ax.set_xlabel('Percentage', fontsize=14, fontweight='bold')
    ax.set_xticklabels([])

    # Add central line (dashed for subtlety)
    ax.axvline(x=50, color=CAI_PRIMARY, linewidth=1.5, linestyle='--', alpha=0.6, zorder=0)

    # Legend (outside the plot area)
    from matplotlib.patches import Patch
    legend_elements = [
        Patch(facecolor=CAI_PRIMARY, label=f'{team1_name} Win ({wins})'),
        Patch(facecolor=TIE_COLOR, label=f'Tie ({ties})'),
        Patch(facecolor=ENEMY_COLOR, label=f'{team2_name} Win ({loses})')
    ]
    ax.legend(handles=legend_elements, loc='upper left', bbox_to_anchor=(1.01, 1), fontsize=12, frameon=True, fancybox=True, ncol=1)

    # Grid
    ax.grid(axis='x', alpha=0.3)
    ax.set_axisbelow(True)

    plt.tight_layout()
    plt.savefig(output_file, dpi=300, bbox_inches='tight')
    print(f"Saved {output_file}")
    plt.close()

def create_combined_visualization(results, matchup, team1_name, team2_name, output_file):
    """Create visualization with main chart only (no overall summary)"""

    machines = ['fortress', 'monolithsentinel', 'reactorwatch', 'hydrocore',
                'securevault', 'docuflow', 'devops', 'notes', 'cowsay', 'pingpong']
    machines = [m for m in machines if m in results]
    n_machines = len(machines)

    if n_machines == 0:
        print(f"No data for {matchup}")
        return

    # Calculate data for main chart
    data = []
    for machine in machines:
        team1_score = results[machine]['team1']
        team2_score = results[machine]['team2']
        total_score = team1_score + team2_score

        if team1_score == team2_score:
            win_pct = 50
            lose_pct = 50
            is_tie = True
        elif total_score == 0:
            win_pct = 50
            lose_pct = 50
            is_tie = True
        else:
            win_pct = (team1_score / total_score) * 100
            lose_pct = (team2_score / total_score) * 100
            is_tie = False

        data.append({
            'machine': machine,
            'team1_score': team1_score,
            'team2_score': team2_score,
            'win_pct': win_pct,
            'lose_pct': lose_pct,
            'is_tie': is_tie
        })

    # Create figure with single chart
    fig, ax1 = plt.subplots(figsize=(14, n_machines * 0.8))

    y_pos = np.arange(len(data))

    for i, d in enumerate(data):
        if d['win_pct'] > 0:
            bars1 = ax1.barh(i, d['win_pct'], color=CAI_PRIMARY, height=0.75)
            # Add subtle shadow effect
            for bar in bars1:
                bar.set_path_effects([patheffects.SimplePatchShadow(offset=(2, -2), shadow_rgbFace='black', alpha=0.3),
                                     patheffects.Normal()])
            if d['win_pct'] > 8:
                txt = ax1.text(d['win_pct']/2, i, f"{d['team1_score']}",
                       ha='center', va='center', color='white', fontweight='bold', fontsize=13)
                txt.set_path_effects([patheffects.withStroke(linewidth=3, foreground=TEXT_DARK, alpha=0.8),
                    patheffects.Normal()])

        if d['lose_pct'] > 0:
            bars2 = ax1.barh(i, d['lose_pct'], left=d['win_pct'], color=ENEMY_COLOR, height=0.75)
            # Add subtle shadow effect
            for bar in bars2:
                bar.set_path_effects([patheffects.SimplePatchShadow(offset=(2, -2), shadow_rgbFace='black', alpha=0.3),
                                     patheffects.Normal()])
            if d['lose_pct'] > 8:
                txt = ax1.text(d['win_pct'] + d['lose_pct']/2, i, f"{d['team2_score']}",
                       ha='center', va='center', color=TEXT_DARK, fontweight='bold', fontsize=13)
                txt.set_path_effects([patheffects.withStroke(linewidth=3, foreground='white', alpha=0.8),
                    patheffects.Normal()])

    ax1.set_yticks(y_pos)
    ax1.set_yticklabels([d['machine'].capitalize() for d in data], fontsize=13, fontweight='bold')
    ax1.set_xlim(0, 100)
    ax1.set_xlabel('Score Distribution', fontsize=14, fontweight='bold')
    ax1.set_xticklabels([])
    ax1.axvline(x=50, color=CAI_PRIMARY, linewidth=1.5, linestyle='--', alpha=0.6, zorder=0)

    from matplotlib.patches import Patch
    legend_elements = [
        Patch(facecolor=CAI_PRIMARY, label=team1_name),
        Patch(facecolor=ENEMY_COLOR, label=team2_name)
    ]
    ax1.legend(handles=legend_elements, loc='upper left', bbox_to_anchor=(1.01, 1), fontsize=12, frameon=True, fancybox=True)
    ax1.grid(axis='x', alpha=0.3)
    ax1.set_axisbelow(True)

    # Remove top and right spines for cleaner look
    ax1.spines['top'].set_visible(False)
    ax1.spines['right'].set_visible(False)

    plt.tight_layout()
    plt.savefig(output_file, dpi=300, bbox_inches='tight')
    print(f"Saved {output_file}")
    plt.close()

def create_paginated_score_bars(matchups: List[Dict[str, object]], output_dir: Path, items_per_page: int = 10):
    """Create paginated score bar visualizations, combining up to 10 matches per image."""

    if not matchups:
        print("No matchups to create score bars.")
        return

    total_matchups = len(matchups)
    total_pages = (total_matchups + items_per_page - 1) // items_per_page  # Ceiling division

    print(f"\nCreating {total_pages} paginated score bar image(s) for {total_matchups} match(es)...")

    for page_num in range(total_pages):
        start_idx = page_num * items_per_page
        end_idx = min(start_idx + items_per_page, total_matchups)
        page_matchups = matchups[start_idx:end_idx]

        # Calculate total height needed for this page
        total_machines = sum(len(m['machines']) for m in page_matchups)
        fig_height = max(6, total_machines * 0.5 + len(page_matchups) * 1.2)  # Reduced: thinner bars need less height

        fig, axes = plt.subplots(len(page_matchups), 1, figsize=(14, fig_height))
        if len(page_matchups) == 1:
            axes = [axes]  # Make it iterable

        for ax_idx, entry in enumerate(page_matchups):
            machines = entry['machines']
            team1_name = entry['team1_name']
            team2_name = entry['team2_name']
            label = entry['label']

            # Order machines
            machine_order = ['fortress', 'monolithsentinel', 'reactorwatch', 'hydrocore',
                           'securevault', 'docuflow', 'devops', 'notes', 'cowsay', 'pingpong']
            ordered_machines = [m for m in machine_order if m in machines]
            extra_machines = [m for m in machines.keys() if m not in machine_order]
            ordered_machines.extend(sorted(extra_machines))

            # Calculate data
            data = []
            for machine in ordered_machines:
                team1_score = machines[machine]['team1']
                team2_score = machines[machine]['team2']
                total_score = team1_score + team2_score

                if team1_score == team2_score:
                    win_pct = 50
                    lose_pct = 50
                elif total_score == 0:
                    win_pct = 50
                    lose_pct = 50
                else:
                    win_pct = (team1_score / total_score) * 100
                    lose_pct = (team2_score / total_score) * 100

                data.append({
                    'machine': machine,
                    'team1_score': team1_score,
                    'team2_score': team2_score,
                    'win_pct': win_pct,
                    'lose_pct': lose_pct,
                })

            ax = axes[ax_idx]
            y_pos = np.arange(len(data))

            # Plot bars
            for i, d in enumerate(data):
                if d['win_pct'] > 0:
                    bars1 = ax.barh(i, d['win_pct'], color=CAI_PRIMARY, height=0.5)  # Thinner bars
                    for bar in bars1:
                        bar.set_path_effects([patheffects.SimplePatchShadow(offset=(2, -2), shadow_rgbFace='black', alpha=0.3),
                                             patheffects.Normal()])
                    if d['win_pct'] > 8:
                        txt = ax.text(d['win_pct']/2, i, f"{d['team1_score']}",
                               ha='center', va='center', color='white', fontweight='bold', fontsize=11)
                        txt.set_path_effects([patheffects.withStroke(linewidth=2, foreground=TEXT_DARK, alpha=0.8),
                            patheffects.Normal()])

                if d['lose_pct'] > 0:
                    bars2 = ax.barh(i, d['lose_pct'], left=d['win_pct'], color=ENEMY_COLOR, height=0.5)  # Thinner bars
                    for bar in bars2:
                        bar.set_path_effects([patheffects.SimplePatchShadow(offset=(2, -2), shadow_rgbFace='black', alpha=0.3),
                                             patheffects.Normal()])
                    if d['lose_pct'] > 8:
                        txt = ax.text(d['win_pct'] + d['lose_pct']/2, i, f"{d['team2_score']}",
                               ha='center', va='center', color=TEXT_DARK, fontweight='bold', fontsize=11)
                        txt.set_path_effects([patheffects.withStroke(linewidth=2, foreground='white', alpha=0.8),
                            patheffects.Normal()])

            ax.set_yticks(y_pos)
            ax.set_yticklabels([d['machine'].capitalize() for d in data], fontsize=11, fontweight='bold')
            ax.set_xlim(0, 100)
            ax.set_xticklabels([])
            ax.axvline(x=50, color=CAI_PRIMARY, linewidth=1.5, linestyle='--', alpha=0.6, zorder=0)
            ax.set_title(label, fontsize=12, fontweight='bold', pad=8)
            ax.grid(axis='x', alpha=0.3)
            ax.set_axisbelow(True)
            ax.spines['top'].set_visible(False)
            ax.spines['right'].set_visible(False)

            # Add legend only to the first subplot
            if ax_idx == 0:
                from matplotlib.patches import Patch
                legend_elements = [
                    Patch(facecolor=CAI_PRIMARY, label=team1_name),
                    Patch(facecolor=ENEMY_COLOR, label=team2_name)
                ]
                ax.legend(handles=legend_elements, loc='upper left', bbox_to_anchor=(1.08, 1), fontsize=10, frameon=True, fancybox=True)

        plt.tight_layout()
        page_suffix = f"_page{page_num + 1}" if total_pages > 1 else ""
        output_file = output_dir / f"ad_scores_combined{page_suffix}.png"
        plt.savefig(output_file, dpi=300, bbox_inches='tight')
        print(f"Saved paginated score bars: {output_file}")
        plt.close()


def create_overall_summary(matchups: List[Dict[str, object]], output_file, team1_name: str, team2_name: str):
    """Create ONE overall summary aggregating ALL matches."""

    if not matchups:
        print("No matchups to create overall summary.")
        return

    # Aggregate wins/ties/losses across ALL matches
    total_wins = 0
    total_ties = 0
    total_loses = 0

    for entry in matchups:
        machines: Dict[str, Dict[str, int]] = entry['machines']
        wins = sum(1 for scores in machines.values() if scores['team1'] > scores['team2'])
        ties = sum(1 for scores in machines.values() if scores['team1'] == scores['team2'])
        loses = sum(1 for scores in machines.values() if scores['team1'] < scores['team2'])

        total_wins += wins
        total_ties += ties
        total_loses += loses

    total = total_wins + total_ties + total_loses
    if total == 0:
        print("No data to create overall summary.")
        return

    win_pct = (total_wins / total) * 100
    tie_pct = (total_ties / total) * 100
    lose_pct = (total_loses / total) * 100

    # Create figure with single horizontal bar
    fig, ax = plt.subplots(figsize=(14, 2.5))

    y_pos = 0

    # Win portion (team 1)
    if win_pct > 0:
        bars1 = ax.barh(y_pos, win_pct, color=CAI_PRIMARY, height=0.75)
        # Add subtle shadow effect
        for bar in bars1:
            bar.set_path_effects([patheffects.SimplePatchShadow(offset=(1, -1), shadow_rgbFace='black', alpha=0.2),
                                 patheffects.Normal()])
        if win_pct > 8:
            txt = ax.text(win_pct/2, y_pos, f'{win_pct:.1f}%\n({total_wins})',
                   ha='center', va='center', color='white', fontweight='bold', fontsize=13)
            txt.set_path_effects([
                patheffects.withStroke(linewidth=3, foreground=TEXT_DARK, alpha=0.8),
                patheffects.Normal()
            ])

    # Tie portion
    if tie_pct > 0:
        bars2 = ax.barh(y_pos, tie_pct, left=win_pct, color=TIE_COLOR, height=0.75)
        # Add subtle shadow effect
        for bar in bars2:
            bar.set_path_effects([patheffects.SimplePatchShadow(offset=(1, -1), shadow_rgbFace='black', alpha=0.2),
                                 patheffects.Normal()])
        if tie_pct > 8:
            txt = ax.text(win_pct + tie_pct/2, y_pos, f'{tie_pct:.1f}%\n({total_ties})',
                   ha='center', va='center', color='white', fontweight='bold', fontsize=13)
            txt.set_path_effects([
                patheffects.withStroke(linewidth=3, foreground=TEXT_DARK, alpha=0.8),
                patheffects.Normal()
            ])

    # Lose portion (opponent)
    if lose_pct > 0:
        bars3 = ax.barh(y_pos, lose_pct, left=win_pct+tie_pct, color=ENEMY_COLOR, height=0.75)
        # Add subtle shadow effect
        for bar in bars3:
            bar.set_path_effects([patheffects.SimplePatchShadow(offset=(1, -1), shadow_rgbFace='black', alpha=0.2),
                                 patheffects.Normal()])
        if lose_pct > 8:
            txt = ax.text(win_pct + tie_pct + lose_pct/2, y_pos, f'{lose_pct:.1f}%\n({total_loses})',
                   ha='center', va='center', color=TEXT_DARK, fontweight='bold', fontsize=13)
            txt.set_path_effects([
                patheffects.withStroke(linewidth=3, foreground='white', alpha=0.8),
                patheffects.Normal()
            ])

    ax.set_xlim(0, 100)
    ax.set_ylim(-0.5, 0.5)
    ax.set_yticks([])
    ax.set_xlabel('Overall Win/Tie/Lose Percentage', fontsize=14, fontweight='bold')
    ax.set_xticklabels([])

    # Add central line (dashed for subtlety)
    ax.axvline(x=50, color=CAI_PRIMARY, linewidth=1.5, linestyle='--', alpha=0.6, zorder=0)

    # Legend (outside the plot area)
    from matplotlib.patches import Patch
    legend_elements = [
        Patch(facecolor=CAI_PRIMARY, label=f'{team1_name} Win ({total_wins})'),
        Patch(facecolor=TIE_COLOR, label=f'Tie ({total_ties})'),
        Patch(facecolor=ENEMY_COLOR, label=f'{team2_name} Win ({total_loses})')
    ]
    ax.legend(handles=legend_elements, loc='upper left', bbox_to_anchor=(1.01, 1), fontsize=12, frameon=True, fancybox=True, ncol=1)

    # Grid
    ax.grid(axis='x', alpha=0.3)
    ax.set_axisbelow(True)

    # Add title
    ax.set_title(f'Overall Summary: {len(matchups)} Match(es), {total} Machine(s) Total',
                 fontsize=16, fontweight='bold', pad=15)

    plt.tight_layout()
    plt.savefig(output_file, dpi=300, bbox_inches='tight')
    print(f"Saved overall summary: {output_file}")
    plt.close()


def main():
    project_root = Path(__file__).resolve().parent
    game_logs_dir = project_root / 'game_logs'
    output_dir = project_root / 'results'
    output_dir.mkdir(parents=True, exist_ok=True)

    # Load team names from config
    config_path = project_root / 'ad_config.yml'
    config_team_names = load_team_names_from_config(config_path)

    if config_team_names:
        print(f"Using team names from config: {config_team_names}")
    else:
        print("No team names found in config, will use names from game logs")

    print("Extracting scores from game logs...")
    results = parse_game_logs(game_logs_dir, config_team_names)

    if not results:
        print("No results found in game logs.")
        return

    print("\nCreating visualizations...")
    matchup_entries: List[Dict[str, object]] = []

    # Collect all matchup entries
    for matchup_key, entry in results.items():
        machines: Dict[str, Dict[str, int]] = entry['machines']
        if not machines:
            continue
        matchup_entries.append(entry)

    if not matchup_entries:
        print("No valid matchups found.")
        return

    # Get team names (use first entry's names, should be consistent across all matches)
    team1_name = matchup_entries[0]['team1_name']
    team2_name = matchup_entries[0]['team2_name']

    # 1. Create individual timeline for each match (handled by create_ad_combined_timeline.py)
    print(f"\nFound {len(matchup_entries)} match(es) with team names: {team1_name} vs {team2_name}")

    # 2. Create paginated score bar visualizations (10 matches per image)
    create_paginated_score_bars(matchup_entries, output_dir, items_per_page=10)

    # 3. Create ONE overall summary for ALL matches
    print("\nCreating overall summary...")
    overall_path = output_dir / 'ad_overall_summary.png'
    create_overall_summary(matchup_entries, overall_path, team1_name, team2_name)

    print("\nDone!")

if __name__ == '__main__':
    main()
