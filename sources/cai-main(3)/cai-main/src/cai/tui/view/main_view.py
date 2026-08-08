"""
MainView -- layout composition, CSS, themes, and help content.

Extracted from CAITerminal so the App class only wires things together.
All functions here are pure or take explicit widget arguments; nothing
imports from ``cai_terminal`` to avoid circular dependencies.
"""

from __future__ import annotations

from typing import TYPE_CHECKING, Any

from textual.app import ComposeResult
from textual.containers import Container, Horizontal
from textual.theme import Theme as TextualTheme
from textual.widgets import Button, Footer, Static, TabbedContent, TabPane

# Lazily imported component types (these are lightweight).
from cai.tui.components.stable_grid import StableTerminalGrid
from cai.tui.components.universal_terminal import UniversalTerminal  # noqa: F401
from cai.tui.components.sidebar import Sidebar
from cai.tui.components.prompt_input import PromptInput
from cai.tui.components.agent_selector_panel import AgentSelectorPanel
from cai.tui.components.agent_creator_panel import AgentCreatorPanel
from cai.tui.components.info_status_bar import InfoStatusBar
from cai.tui.components.graph_canvas import CTRCanvas

if TYPE_CHECKING:
    pass  # avoid circular imports with cai_terminal


# ---------------------------------------------------------------------------
# CSS -- previously the CAITerminal.CSS class variable (~1 200 lines)
# ---------------------------------------------------------------------------

CAI_TERMINAL_CSS: str = """
/* MINIMAL CSS FOR DEBUGGING TAB ISSUE */

Screen {
    background: $background;
}

/* Global Screen Styling */
Screen {
    background: $background;
}

/* Force all text to be visible */

/* Ensure button text is visible */
Button { text-opacity: 1.0 !important; }

Button Label {
    text-opacity: 1.0 !important;
}

/* Force button label visibility globally */
Button .button--label {
    color: $text !important;
    text-opacity: 1.0 !important;
}

/* Fix button content visibility */
Sidebar Button.agent-item {
    content-align: left middle !important;
}

Sidebar Button.agent-item > * {
    visibility: visible !important;
    display: block !important;
}

/* Force text color for all button states */
Sidebar Button.agent-item,
Sidebar Button.agent-item:hover,
Sidebar Button.agent-item:focus {
    text-opacity: 1.0 !important;
}

/* Ensure the button renders its label correctly */
Sidebar Button.agent-item > Label,
Sidebar Button.agent-item > Static { color: $text !important; text-opacity: 1.0 !important; visibility: visible !important; }

/* Debug: Force specific text rendering for buttons */
Sidebar Button.agent-item {
    text-style: none !important;
}

/* Ensure the button content container is visible */
Sidebar Button.agent-item > * > * {
    color: $text !important;
    text-opacity: 1.0 !important;
}

/* Modern Scrollbar Design */
ScrollBar {
    background: $surface;
    color: $primary 30%;
}

ScrollBar:hover {
    color: $primary;
}

/* Streaming Message Styles - Refined */
StreamingMessage,
IntegratedStreamingMessage {
    background: $surface;
    padding: 1 2;
    margin: 1 2;
    border: none;
    border-left: solid $primary 30%;
}

InlineStreamingMessage {
    background: transparent;
    padding: 0;
    margin: 0;
}

/* Terminal Output Area - Enhanced */
.terminal-richlog {
    background: $surface;
    padding: 1 2;
    color: $text;
}

.terminal-content {
    background: $background;
    border: none;
}

/* Layout Containers - Modern Design */
#app-layout {
    background: $background;
    width: 100%;
    height: 100%;
    layout: horizontal;
    padding: 0;
    margin: 0;
}

#main-container {
    background: $background;
    padding: 0;
    margin: 0;
    width: 100%;
    height: 100%;
}


/* TabbedContent Styling */
TabbedContent {
    width: 100%;
    height: 100%;
}

/* Tab Bar - Fix text visibility issue */
Tabs {
    height: 3;
    dock: top;
    background: $surface;
}

/* Default tab style with visible text */
Tab {
    padding: 0 3;
    text-align: center;
    min-width: 12;
    color: #cccccc !important;
    text-opacity: 1.0 !important;
}

/* Active tab override */
Tab.-active {
    color: white !important;
    background: #0178d4 !important;
    text-opacity: 1.0 !important;
}

/* Tab Bar container styling */
#main-tabs Tabs {
    background: transparent;
    height: 3;
    width: 100%;
}

/* Main tabs - exact copy of sidebar approach with different colors */
#main-tabs Tab {
    height: 3;
    max-height: 3;
    background: transparent;
    padding: 0 2;
    margin: 0 1 0 0;
    border: none;
    color: #ffffff !important;
    text-opacity: 1.0 !important;
    content-align: center middle;
    text-align: center;
}

/* Active tab for main container */
#main-tabs Tab.-active {
    color: #ffffff !important;
    background: #0178d4;
    text-style: bold;
    text-opacity: 1.0 !important;
    border: none;
    margin: 0 1 0 0;
    height: 3;
}

/* Hover state */
#main-tabs Tab:hover {
    color: #ffffff !important;
    background: rgba(1, 120, 212, 0.3);
    text-opacity: 1.0 !important;
    border: none;
}

/* Force all tab text to be visible and centered */
#main-tabs Tab * {
    color: #ffffff !important;
    text-opacity: 1.0 !important;
    background: transparent !important;
    text-align: center !important;
    width: 100%;
    height: 100%;
    content-align: center middle;
}

#main-tabs Tab.-active * {
    color: #ffffff !important;
    text-opacity: 1.0 !important;
}

#main-tabs Tab:hover * {
    color: #ffffff !important;
    text-opacity: 1.0 !important;
}

/* Ensure tab labels are properly displayed */
#main-tabs Tab Label {
    color: #ffffff !important;
    text-opacity: 1.0 !important;
    background: transparent !important;
    width: 100%;
    height: 100%;
    content-align: center middle;
    text-align: center;
}

#main-tabs Tab.-active Label {
    color: #ffffff !important;
    text-opacity: 1.0 !important;
}

#main-tabs Tab:hover Label {
    color: #ffffff !important;
    text-opacity: 1.0 !important;
}

/* Content Switcher */
ContentSwitcher {
    width: 100%;
    height: 100%;
    background: $background;
}

/* Tab Panes */
TabPane {
    width: 100%;
    height: 100%;
    padding: 0;
    layout: vertical;
}

/* Terminal Tab Content */
#terminal {
    width: 100%;
    height: 100%;
}

#terminal StableTerminalGrid {
    width: 100%;
    height: 1fr;
}

#terminal InfoStatusBar {
    height: 2;
    dock: bottom;
}

#terminal #input-area {
    height: 3;
    dock: bottom;
    background: $surface;
    border-top: solid $primary 30%;
    padding: 0 2;
}

/* CTR Tab Content */
#ctr {
    width: 100%;
    height: 100%;
}

#ctr CTRCanvas {
    width: 100%;
    height: 100%;
}

/* Ensure info status bar is visible */
#info-status-bar {
    height: 2;
    width: 100%;
    min-height: 2;
    max-height: 2;
    dock: bottom;
}

/* Remove gaps between terminals for unified action bar */
StableTerminalGrid.layout-triple {
    grid-gutter: 0 0;
}

/* Compact layout for 4+ terminals */
StableTerminalGrid.many-terminals {
    grid-gutter: 0 0;
    padding: 0;
}

/* Adjust terminal content area for 4+ terminals */
.many-terminals UniversalTerminal {
    padding: 0;
}

/* Smaller info bar for 4+ terminals */
.many-terminals InfoStatusBar {
    height: 1;
    min-height: 1;
}

/* Hide scrollbars in many-terminals mode but NOT ActualActionBar */
.many-terminals ScrollBar {
    display: none !important;
}

/* Don't hide ActualActionBar which extends VerticalScroll */
.many-terminals ActualActionBar {
    display: block !important;
}


/* Modern Sidebar Design */
#sidebar {
    width: 32;
    background: $surface;
    border-right: solid $primary 50%;
    dock: left;
    display: none;
    padding: 0;
}

#sidebar.sidebar-visible {
    display: block;
}

.sidebar-header {
    height: 3;
    background: $surface;
    color: $primary;
    padding: 0 1;
    text-align: center;
    text-style: bold;
    border-bottom: solid $primary 50%;
    margin: 0;
}

.sidebar-list {
    height: 1fr;
    background: $surface;
    color: $text;
    padding: 0;
    margin: 0;
}

/* Modern List Item Styling */
ListItem {
    padding: 1 2;
    margin: 0;
    background: $surface;
    border: none;
    border-bottom: tall $primary 10%;
    color: $text;
}

ListItem Label {
    color: $text !important;
    background: transparent !important;
}

ListItem:hover {
    background: $surface-lighten-2;
    border: none;
    border-bottom: tall $primary 10%;
    color: $secondary;
}

ListItem:hover Label {
    color: $secondary !important;
}

ListItem.-selected,
ListItem.--highlight {
    background: $surface-lighten-2;
    border: none;
    border-left: thick $primary;
    border-bottom: tall $primary 10%;
    color: $primary;
    text-style: bold;
}

ListItem.-selected Label,
ListItem.--highlight Label {
    color: $primary !important;
}

/* Sidebar Content Container */
#sidebar-content {
    height: 100%;
    background: $surface;
    padding: 0;
    margin: 0;
}

/* Tabbed Content Styles for Sidebar */
TabbedContent {
    background: $surface;
    height: 100%;
    border: none;
}

TabbedContent > ContentTabs {
    background: $surface;
    height: 3;
    padding: 0;
}

/* Only set padding and sizing */
TabbedContent Tab {
    padding: 1 2;
    min-width: 12;
}

TabbedContent > TabContent {
    background: $surface;
    padding: 0;
    height: 1fr;
}

/* Sidebar Scroll Containers */
VerticalScroll.sidebar-list,
VerticalScroll.queue-list,
VerticalScroll.state-list {
    background: $surface;
    padding: 0;
    margin: 0;
}

/* Override sidebar ListItem styles to ensure text visibility */
Sidebar ListItem {
    background: $surface !important;
}

Sidebar ListItem Label {
    color: $secondary !important;
    text-opacity: 1.0 !important;
    background: transparent !important;
}

/* Sidebar separators */
Sidebar .agent-separator {
    width: 100% !important;
    height: 0 !important;
    margin: 0 !important;
    border: none !important;
    background: transparent !important;
}

/* Override sidebar list styles */
Sidebar .sidebar-list {
    background: $surface !important;
    padding: 0 !important;
    margin: 0 !important;
}

Sidebar .queue-list {
    background: $surface !important;
    padding: 0 !important;
    margin: 0 !important;
}

Sidebar .state-list {
    background: $surface !important;
    padding: 0 !important;
    margin: 0 !important;
}

/* Main Content Area - Clean Design */
#content-area {
    height: 1fr;
    background: $background;
}

/* Terminal Grid Container - Modern Spacing */
#terminal-grid-container {
    width: 100%;
    height: 1fr;
    background: $background;
    padding: 0;
    overflow-y: auto;
    scrollbar-size: 1 1;
    scrollbar-color: #529d86;
    scrollbar-background: #2e4f46;
}

.terminal-grid {
    height: 100%;
    width: 100%;
    layout: grid;
    grid-gutter: 1 2;
}

/* Universal Terminal - Enhanced Design with Effects */
.grid-terminal {
    width: 1fr;
    height: 1fr;
    border: none;
    background: $background;
    min-height: 10;
    min-width: 40;
    padding: 0;
}

.grid-terminal:hover {
    border: none;
    background: $background;
}

.terminal-container {
    height: 100%;
    width: 100%;
    background: transparent;
}

/* Terminal Header Bar - Modern Glass Effect */
.terminal-header-bar {
    height: 2;
    min-height: 2;
    max-height: 2;
    background: $surface;
    border-bottom: solid $primary 50%;
    layout: horizontal;
    padding: 0 1;
}

.terminal-status {
    width: auto;
    color: $text-muted;
    padding: 0 2 0 0;
    content-align: left middle;
}

.terminal-header {
    width: 1fr;
    color: $text;
    padding: 0 1;
    content-align: left middle;
    text-style: bold;
}

/* Role Indicator - Modern Icons */
.role-indicator {
    width: 3;
    content-align: center middle;
    padding: 0 1;
    text-style: bold;
}

.role-indicator.inactive {
    color: $text-muted;
}
.role-indicator.main {
    color: $primary;
    text-style: bold reverse;
}
.role-indicator.agent {
    color: $secondary;
    text-style: bold;
}
.role-indicator.monitor {
    color: $warning;
    text-style: bold;
}
.role-indicator.logger {
    color: $error;
    text-style: bold;
}

/* Terminal Output - Enhanced Readability */
.terminal-output {
    height: 1fr;
    background: $surface;
    color: $text;
    padding: 1;
    scrollbar-size: 1 1;
    scrollbar-color: #529d86;
    scrollbar-background: #2e4f46;
}

/* Terminal States - Modern Focus Effects */
.terminal-focused {
    border: none !important;
}

.terminal-focused .terminal-header-bar {
    background: $surface;
    border-bottom: solid $secondary !important;
}

.terminal-active .terminal-header-bar {
    background: $surface;
}

/* Layout Modes - Responsive Design */
.layout-single .terminal-grid {
    grid-size: 1 1;
}

.layout-split .terminal-grid {
    grid-size: 2 1;
    grid-gutter: 2 3;
}

.layout-grid .terminal-grid {
    grid-gutter: 2 3;
}

.layout-vertical .terminal-grid {
    layout: vertical;
    grid-gutter: 2 0;
}

.layout-horizontal .terminal-grid {
    layout: horizontal;
    grid-gutter: 0 3;
}

/* Terminal Role Styles - Modern Design without borders */
.role-main {
    border: none !important;
}

.role-main .terminal-header-bar {
    background: $surface;
    border-bottom: solid $primary;
}

.role-main .terminal-header {
    color: $primary;
}

.role-agent {
    border: none !important;
}

.role-agent .terminal-header-bar {
    background: $surface;
    border-bottom: solid $secondary;
}

.role-agent .terminal-header {
    color: $secondary;
}

.role-monitor {
    border: none !important;
}

.role-monitor .terminal-header-bar {
    background: $surface;
    border-bottom: solid $warning;
}

.role-monitor .terminal-header {
    color: $warning;
}

.role-logger {
    border: none !important;
}

.role-logger .terminal-header-bar {
    background: $surface;
    border-bottom: solid $error;
}

.role-logger .terminal-header {
    color: $error;
}

.role-empty {
    border: none !important;
}

/* Parallel Grid - Modern Layout */
.parallel-grid {
    height: 100%;
    width: 100%;
    background: $background;
}

.parallel-row {
    height: 1fr;
    width: 100%;
    layout: horizontal;
    padding: 1;
}

.parallel-column {
    height: 100%;
    width: 100%;
    layout: vertical;
    padding: 1;
}

/* Agent Terminal - Enhanced Design */
.agent-terminal {
    width: 1fr;
    height: 100%;
    border: thick $primary 50%;
    margin: 0 1;
    background: $surface;
}

.agent-terminal:hover {
    border: thick $primary-lighten-1;
}

.agent-terminal-content {
    height: 100%;
    background: $surface;
}

.agent-header {
    height: 3;
    background: $surface-lighten-1;
    padding: 0 2;
    text-align: center;
    text-style: bold;
    border-bottom: tall $primary 50%;
    color: $text;
}

/* Streaming Labels - Clean Design */
Label#stream-* {
    width: 100%;
    height: 1;
    padding: 0 2;
    background: transparent;
    color: $text;
}

/* Info Status Bar - Modern Information Display */
InfoStatusBar {
    height: 2;
    width: 100%;
    margin: 0;
    background: #0a0a0a;
    border-top: solid #03fcb1;
    padding: 0;
    dock: bottom;
}

InfoStatusBar Static {
    color: #c8ff00 !important;
    background: transparent;
    height: 100%;
}

InfoStatusBar .info-section {
    color: #c8ff00 !important;
    height: 100%;
}

InfoStatusBar .separator {
    color: #529d86 !important;
}

/* Action Bar - Modern Status Display */
ActualActionBar {
    margin: 0;
    background: $surface;
    border: solid $primary 20%;
    padding: 0;
}

/* Force action bar height for 4+ terminals */
.many-terminals ActualActionBar,
StableTerminalGrid.many-terminals ActualActionBar {
    height: 5 !important;
    min-height: 5 !important;
    max-height: 5 !important;
}

/* Terminal Output Optimization */
.terminal-output {
    height: 1fr;
    margin-bottom: 0;
}

/* Universal Terminal Layout */
UniversalTerminal {
    height: 100%;
    layout: vertical;
}

/* Terminal Container Responsiveness */
.terminal-container {
    height: 1fr;
}

/* Agent Output - Enhanced Readability */
.agent-output {
    height: 1fr;
    background: $surface;
    color: $text;
    padding: 1 2;
    scrollbar-size: 1 1;
    scrollbar-color: #529d86;
    scrollbar-background: #2e4f46;
}

/* Tool Panel - Modern Card Design */
.tool-panel-container {
    margin: 1 2;
    background: $surface-lighten-1;
    border: tall $primary 20%;
    padding: 1;
}

.tool-content {
    width: 100%;
    color: $text;
}

/* Input Area - Modern Command Line */
#input-area {
    height: 3;
    width: 100%;
    background: $surface;
    border: solid $primary 50%;
    padding: 0 2;
    layout: horizontal;
    align: left middle;
}

#main-input {
    height: 1;
    width: 100%;
    background: transparent;
    padding: 0;
    layout: horizontal;
}

#main-input:focus {
    background: transparent;
}

/* Prompt Input Styles */
PromptInput {
    height: 1;
    width: 100%;
    background: transparent;
    padding: 0;
    layout: horizontal;
    align: left middle;
}

PromptInput #prompt-prefix {
    color: $primary;
    text-style: bold;
    background: transparent;
    width: auto;
    padding: 0 0 0 0;
    margin: 0 1 0 0;
    height: 1;
    content-align: left middle;
}

PromptInput #prompt-input-field {
    background: transparent !important;
    color: $text !important;
    border: none !important;
    width: 1fr;
    padding: 0 !important;
    height: 1;
}

PromptInput #prompt-input-field:focus {
    background: transparent !important;
    border: none !important;
    color: $text !important;
}

/* Agent Selector Panel - Modern Modal Design */
#agent-selector-panel {
    layer: overlay;
    width: 100%;
    height: 100%;
    display: none;
}

#agent-selector-panel.visible {
    display: block;
}

#agent-selector-overlay {
    width: 100%;
    height: 100%;
    background: rgba(14, 15, 17, 0.9);
    align: center middle;
}

#agent-selector-content {
    width: 60;
    max-height: 80%;
    background: $surface-lighten-1;
    border: thick $primary 50%;
    padding: 2;
}

#selector-header {
    height: 3;
    background: transparent;
    color: $text;
    text-align: center;
    text-style: bold;
    border-bottom: tall $primary 50%;
    margin-bottom: 2;
    padding: 1;
}

#prompt-preview {
    height: auto;
    padding: 1 2;
    margin-bottom: 2;
    color: $text;
    background: $surface;
    border: tall $primary 20%;
}

#agent-list-container {
    height: 1fr;
    overflow-y: auto;
    background: $surface;
    border: tall $primary 20%;
    padding: 2;
    margin-bottom: 2;
    scrollbar-size: 1 1;
    scrollbar-color: #529d86;
    scrollbar-background: #2e4f46;
}

#agent-list-container Checkbox {
    width: 100%;
    margin-bottom: 1;
    color: $text;
    padding: 0 1;
}

#agent-list-container Checkbox:hover {
    background: $surface;
    color: $text;
}

#agent-list-container Checkbox:focus {
    background: $surface-lighten-1;
    border: tall $primary 50%;
}

#selector-buttons {
    height: 3;
    align: center middle;
}

#selector-buttons Button {
    margin: 0 2;
    background: $surface;
    color: $text;
    border: tall $primary 20%;
    padding: 0 3;
    text-style: bold;
}

#selector-buttons Button:hover {
    background: $primary;
    color: $background;
    border: tall $primary;
}

#selector-buttons Button:focus {
    background: $primary-lighten-1;
    color: $background;
}

/* Footer Enhancement - Modern Status Bar */
Footer {
    background: $surface-lighten-1;
    color: $text-muted;
    border-top: solid $primary 50%;
    height: 2;
    padding: 0 2;
    layout: horizontal;
}

Footer > .footer--key {
    background: $surface;
    color: $text;
    text-style: bold;
    margin: 0 1;
    padding: 0 1;
    border: round $primary 20%;
}

Footer > .footer--key-ready {
    background: $primary-darken-1;
    color: $background;
}

Footer > .footer--description {
    color: $text-muted;
    margin: 0 1 0 0;
}

/* Modern Button Styles */
Button {
    background: $surface;
    color: $text;
    border: solid $primary 20%;
    text-style: none;
    padding: 0 2;
    margin: 0 1;
}

Button:hover {
    background: $surface-lighten-1;
    color: $primary;
    border: solid $primary;
}

Button:focus {
    background: $primary 30%;
    color: $primary;
    border: solid $primary;
}

Button.-primary {
    background: $primary;
    color: $background;
    border: solid $primary;
}

Button.-primary:hover {
    background: $primary-lighten-1;
    border: solid $primary-lighten-1;
}

/* Top bar container */
#top-bar {
    height: 3;
    width: 100%;
    background: $surface;
    dock: top;
}

/* Sidebar toggle button integrated in top bar */
.sidebar-toggle {
    width: 8;
    height: 3;
    background: transparent !important;
    border: none !important;
    color: #ffffff !important;
    text-align: center;
    content-align: center middle;
    text-style: bold;
    margin: 0 1 0 0;
    text-opacity: 1.0 !important;
    outline: none !important;
}

.sidebar-toggle:hover {
    background: rgba(1, 120, 212, 0.3) !important;
    border: none !important;
    color: #ffffff !important;
    text-opacity: 1.0 !important;
    outline: none !important;
}

/* Tab headers container - takes remaining space in top bar */
#tab-headers {
    width: 1fr;
    height: 3;
    background: transparent;
    layout: horizontal;
}

/* Main tabs container - full height below top bar */
#main-tabs {
    width: 100%;
    height: 1fr;
}

/* Hide the default tab bar since we're creating custom ones */
#main-tabs Tabs {
    display: none;
}

/* Custom tab buttons */
.custom-tab {
    height: 3;
    background: transparent;
    padding: 0 2;
    margin: 0 1 0 0;
    border: none;
    color: #ffffff !important;
    text-opacity: 1.0 !important;
    content-align: center middle;
    text-align: center;
}

.custom-tab:hover {
    color: #ffffff !important;
    background: rgba(1, 120, 212, 0.3);
    text-opacity: 1.0 !important;
    border: none;
}

.custom-tab.active-tab {
    color: #ffffff !important;
    background: #0178d4;
    text-style: bold;
    text-opacity: 1.0 !important;
    border: none;
}

/* Top bar when sidebar is collapsed - move entire bar left */
#top-bar.sidebar-collapsed {
    margin-left: -32;  /* Move left by sidebar width */
}

/* App close button - styled with red/maroon theme for intuitive close action */
.app-close-button {
    width: 9;
    min-width: 9;
    max-width: 9;
    height: 3;
    background: transparent;
    border: none;
    color: #ffffff;
    text-align: center;
    content-align: center middle;
    text-style: bold;
    margin: 0;
    padding: 0;
    outline: none !important;
}

.app-close-button:hover {
    background: rgba(200, 60, 60, 0.7);
    border: none;
    color: #ffffff;
}

/* Add terminal button - styled to match other top bar buttons */
.add-terminal-button {
    width: 8;
    min-width: 8;
    max-width: 8;
    height: 3;
    background: transparent;
    border: none;
    color: #ffffff;
    text-align: center;
    content-align: center middle;
    text-style: bold;
    margin: 0 1 0 0;
    padding: 0;
    outline: none !important;
    text-opacity: 1.0 !important;
}

.add-terminal-button:hover {
    background: rgba(1, 120, 212, 0.3);
    border: none;
    color: #ffffff;
    text-opacity: 1.0 !important;
}

/* Help section styles */
#help-content {
    width: 100%;
    height: 100%;
    padding: 0;
    background: $surface;
    border-top: solid $border;
    layout: vertical;
}

.help-title {
    color: $text;
    text-align: center;
    padding: 1 0 1 0;
    margin: 0;
    background: transparent;
    border-bottom: solid $border;
}

#help-scrollable-content {
    width: 100%;
    height: 1fr;
    padding: 1 2 2 2;
    background: $surface;
    overflow-y: auto;
    scrollbar-size: 1 1;
    scrollbar-color: #529d86;
    scrollbar-background: #2e4f46;
}

#help-columns {
    width: 100%;
    height: auto;
    layout: horizontal;
}

.help-column {
    width: 50%;
    height: auto;
    padding: 1;
}

#help-left-column {
    border-right: solid $border;
    padding-right: 2;
}

#help-right-column {
    padding-left: 2;
}

.help-content {
    color: $text;
    text-align: left;
    padding: 0;
    margin: 0;
    background: transparent;
    max-width: 100%;
}

.help-protips {
    color: $text;
    text-align: left;
    padding: 1 0 1 0;
    margin: 0;
    background: transparent;
    border-top: solid $border;
    margin-top: 2;
    max-width: 100%;
}

"""


# ---------------------------------------------------------------------------
# Widget composition -- called from CAITerminal.compose()
# ---------------------------------------------------------------------------

def compose_main_layout() -> ComposeResult:
    """Yield the full widget tree for CAITerminal.compose().

    This is a standalone generator so the App class stays slim.
    """
    with Container(id="app-layout"):
        # Sidebar
        yield Sidebar(id="sidebar")

        # Main content container with tabs
        with Container(id="main-container"):
            # Top bar with toggle button, tab headers, and close button
            with Horizontal(id="top-bar"):
                yield Static("\u2630", id="sidebar-toggle-btn", classes="sidebar-toggle")
                with Container(id="tab-headers"):
                    yield Button("Terminal", id="tab-terminal-btn", classes="custom-tab active-tab")
                    add_btn = Static("Add +", id="add-terminal-btn", classes="add-terminal-button")
                    add_btn.tooltip = "Add new terminal"
                    yield add_btn
                    yield Button("Graph", id="tab-graph-btn", classes="custom-tab")
                    yield Button("Help", id="tab-help-btn", classes="custom-tab")
                close_btn = Static("\u00d7", id="app-close-btn", classes="app-close-button")
                close_btn.tooltip = "Close CAI"
                yield close_btn

            # Main tabs content
            with TabbedContent(initial="terminal", id="main-tabs"):
                with TabPane("Terminal", id="terminal"):
                    yield StableTerminalGrid(id="terminal-grid-container")
                    yield InfoStatusBar(id="info-status-bar", terminal_number=1)
                    yield Container(
                        PromptInput(prompt="CAI>", id="main-input"),
                        id="input-area",
                    )

                with TabPane("Graph", id="ctr"):
                    yield CTRCanvas(id="ctr-canvas")

                with TabPane("Help", id="help"):
                    with Container(id="help-content"):
                        yield Static(
                            "[bold cyan]CAI Quick Start Guide[/bold cyan]",
                            id="help-title",
                            classes="help-title",
                            markup=True,
                        )
                        with Container(id="help-scrollable-content"):
                            with Horizontal(id="help-columns"):
                                with Container(id="help-left-column", classes="help-column"):
                                    yield Static(
                                        get_help_basic_content(),
                                        id="help-basic",
                                        classes="help-content",
                                        markup=True,
                                    )
                                with Container(id="help-right-column", classes="help-column"):
                                    yield Static(
                                        get_help_advanced_content(),
                                        id="help-advanced",
                                        classes="help-content",
                                        markup=True,
                                    )
                            yield Static(
                                get_help_protips_content(),
                                id="help-protips",
                                classes="help-protips",
                                markup=True,
                            )

    # Overlay panels
    yield AgentSelectorPanel(id="agent-selector-panel")
    yield AgentCreatorPanel(id="agent-creator-panel")
    yield Footer()


# ---------------------------------------------------------------------------
# Theme registration
# ---------------------------------------------------------------------------

def register_cai_themes(register_fn: Any) -> None:
    """Register CAI-specific themes. *register_fn* is ``App.register_theme``."""
    nature = TextualTheme(
        name="nature",
        primary="#00ff9c",
        accent="#00ff9c",
        foreground="#e6ffe6",
        background="#001f1a",
        surface="#01342c",
        panel="#01342c",
        success="#4CAF50",
        warning="#F5A623",
        error="#FF5A5F",
        dark=True,
        variables={"graph-node": "#4CAF50"},
    )
    register_fn(nature)

    alias = TextualTheme(
        name="alias-dark",
        primary="#FF4D4D",
        accent="#00D1B2",
        foreground="#E8F1F2",
        background="#0B1E24",
        surface="#102A31",
        panel="#0F242A",
        success="#21C36B",
        warning="#F4C430",
        error="#FF4D4D",
        dark=True,
        variables={"graph-node": "#00D1B2", "border": "#184046"},
    )
    register_fn(alias)


# ---------------------------------------------------------------------------
# Tab appearance helper
# ---------------------------------------------------------------------------

def update_tab_appearance(query_one_fn: Any, active_tab: str) -> None:
    """Update CSS classes on custom tab buttons.

    *query_one_fn* is ``App.query_one`` or equivalent.
    """
    try:
        terminal_btn = query_one_fn("#tab-terminal-btn", Button)
        graph_btn = query_one_fn("#tab-graph-btn", Button)
        help_btn = query_one_fn("#tab-help-btn", Button)

        terminal_btn.remove_class("active-tab")
        graph_btn.remove_class("active-tab")
        help_btn.remove_class("active-tab")

        mapping = {"terminal": terminal_btn, "ctr": graph_btn, "help": help_btn}
        btn = mapping.get(active_tab)
        if btn:
            btn.add_class("active-tab")
    except Exception:
        pass


# ---------------------------------------------------------------------------
# Help content generators
# ---------------------------------------------------------------------------

def get_help_basic_content() -> str:
    """Left-column help content."""
    return """[bold yellow]Initial Setup[/bold yellow]
[cyan]Configure your API Key:[/cyan]
\u2022 Go to [bold]Sidebar -> Keys[/bold] tab
\u2022 Click [bold green]Add New Key[/bold green]
\u2022 Enter: [bold]ALIAS_API_KEY[/bold] = your_alias_api_key_here
\u2022 Click [bold green]Save[/bold green] button
\u2022 [dim]Alternative: You can add other providers API_KEYS the same way[/dim]

[bold yellow]Select Your Model[/bold yellow]
[cyan]Choose the right model:[/cyan]
\u2022 In terminal header, click [bold]model[/bold] dropdown
\u2022 Select: [bold green]alias1[/bold green] (recommended)
\u2022 [dim]Alternative: Use command[/dim] [bold]/model alias1[/bold]
\u2022 [dim]alias1 provides optimal performance and cost balance[/dim]

[bold yellow]Choose an Agent[/bold yellow]
[cyan]Pick your AI assistant:[/cyan]
\u2022 Click [bold]agent[/bold] dropdown in terminal header and browse available agents
\u2022 [dim]Recommendation:[/dim] Use [bold]selection_agent[/bold] (or [bold]/agent 17[/bold]) to get a recommendation based on your task
\u2022 [dim]List all agents:[/dim] [bold]/agent list[/bold]
\u2022 [dim]Alternative: Use command[/dim] [bold]/agent agent_name[/bold]


[bold yellow]Add New Terminal[/bold yellow]
[cyan]Open another workspace quickly:[/cyan]
\u2022 Click [bold]Add +[/bold] on the top bar
\u2022 Creates a new terminal with model [bold]alias1[/bold] and agent [bold]redteam_agent[/bold]
\u2022 Tooltip shows: [bold]Add new terminal[/bold]


[bold yellow]Start Chatting[/bold yellow]
[cyan]Begin your conversation:[/cyan]
\u2022 Type in the input field at bottom: [bold]CAI>[/bold]
\u2022 Press [bold]Enter[/bold] to send your prompt
\u2022 You can send another prompt while the first one is being processed, and it will be queued automatically.
\u2022 Use [bold]/help[/bold] for available commands
"""


def get_help_advanced_content() -> str:
    """Right-column help content."""
    return """[bold yellow]Interface Overview[/bold yellow]
[cyan]Main sections explained:[/cyan]
\u2022 [bold]Sidebar[/bold]:
  - [dim]Agents:[/dim] Browse and select AI assistants
  - [dim]Queue:[/dim] Manage prompt batches
  - [dim]Keys:[/dim] Configure API credentials
  - [dim]Stats:[/dim] View conversation stats
\u2022 [bold]Terminal[/bold]: Chat interface and command execution
  - [dim]Add +:[/dim] Create a new terminal (defaults: [bold]alias1[/bold] + [bold]redteam_agent[/bold])
\u2022 [bold]Graph[/bold]: Visual conversation flow representation
\u2022 [bold]Help[/bold]: You are here!

[bold yellow]Advanced Features[/bold yellow]
[cyan]Power user capabilities:[/cyan]
\u2022 [bold]Teams[/bold]: Try collaborative agent groups
  - [dim]Use for complex multi-step tasks[/dim]
  - [dim]Combine different agent specialties[/dim]

\u2022 [bold]Stats Monitoring[/bold]: Track conversation metrics
  - [dim]View costs, tokens...[/dim]
  - [dim]Monitor conversation history[/dim]

\u2022 [bold]Graph Visualization[/bold]: Understand conversation flow
  - [dim]See agent interactions visually[/dim]
  - [dim]Track conversation branches[/dim]

[bold yellow]Essential Shortcuts[/bold yellow]
[cyan]Most used hotkeys:[/cyan]
\u2022 [bold]Ctrl+S[/bold]: Toggle sidebar
\u2022 [bold]Ctrl+Q[/bold]: Exit CAI
\u2022 [bold]Ctrl+L[/bold]: Clear terminals
\u2022 [bold]Ctrl+N/B[/bold]: Next/Previous terminal
\u2022 [bold]Ctrl+E[/bold]: Close current terminal
"""


def get_help_protips_content() -> str:
    """Footer help content."""
    return """[bold green]Pro Tips[/bold green]
[cyan]Expert recommendations:[/cyan]
- Start with [bold]alias1[/bold] model and explore different agents to find your perfect AI assistant!
- Use [bold]/agent list[/bold] to explore all available capabilities and specialties
- Try different agents in parallel for specialized tasks and comprehensive analysis
- You can add at the end of the prompt or command: "t1", "t2", "t3", etc. or "all" to specify the terminal
- Use Teams feature for complex multi-agent workflows and collaborative problem-solving
- Use the Graph view to understand conversation flow and agent interactions visually

[dim]Need more help? Check the sidebar sections or use /help command in terminal.[/dim]"""
