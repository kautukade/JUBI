"""
CAI TUI Configuration Management
"""

import os
import json
from pathlib import Path
from typing import Dict, Any, Optional

class TUIConfig:
    """Manages TUI configuration including theme preferences"""
    
    def __init__(self):
        self.config_dir = Path.home() / ".cai"
        self.config_file = self.config_dir / "tui_config.json"
        self.config = self._load_config()
    
    def _load_config(self) -> Dict[str, Any]:
        """Load configuration from file"""
        if self.config_file.exists():
            try:
                with open(self.config_file, 'r') as f:
                    return json.load(f)
            except Exception:
                pass
        return {}
    
    def _save_config(self) -> None:
        """Save configuration to file"""
        try:
            self.config_dir.mkdir(parents=True, exist_ok=True)
            with open(self.config_file, 'w') as f:
                json.dump(self.config, f, indent=2)
        except Exception:
            pass
    
    def get_theme(self) -> Optional[str]:
        """Get saved theme preference"""
        # Environment variable takes precedence
        env_theme = os.getenv("CAI_THEME")
        if env_theme:
            return env_theme
        # Otherwise use saved config
        return self.config.get("theme")
    
    def set_theme(self, theme: str) -> None:
        """Save theme preference"""
        self.config["theme"] = theme
        self._save_config()
    
    def get(self, key: str, default: Any = None) -> Any:
        """Get configuration value"""
        return self.config.get(key, default)
    
    def set(self, key: str, value: Any) -> None:
        """Set configuration value"""
        self.config[key] = value
        self._save_config()