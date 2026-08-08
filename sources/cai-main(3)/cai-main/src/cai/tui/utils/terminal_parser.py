"""
Simple terminal parser utility
"""

import re
from typing import Tuple, List, Optional


def parse_terminal_target(args: List[str]) -> Tuple[List[str], Optional[int]]:
    """
    Parse terminal target from command arguments.
    
    Looks for terminal specifiers like 't1', 'T2', etc. at the end of arguments.
    Also preserves 'all' for broadcast to all terminals.
    
    Args:
        args: List of command arguments
        
    Returns:
        Tuple of (cleaned_args, terminal_number)
        - cleaned_args: Arguments with terminal specifier removed (but 'all' preserved)
        - terminal_number: Terminal number if found, None otherwise
        
    Examples:
        ['gpt-4o', 't1'] -> (['gpt-4o'], 1)
        ['select', 'agent_name', 'T2'] -> (['select', 'agent_name'], 2)
        ['ping', '192.168.1.1', 'all'] -> (['ping', '192.168.1.1', 'all'], None)
        ['list'] -> (['list'], None)
    """
    if not args:
        return args, None
        
    last_arg = args[-1]
    
    # Check if it's "all" - preserve it in the args
    if last_arg.lower() == 'all':
        return args, None
    
    # Match terminal specifiers: t1, T1, t2, T2, etc.
    match = re.match(r'^[tT](\d+)$', last_arg)
    if match:
        terminal_number = int(match.group(1))
        return args[:-1], terminal_number
    
    return args, None