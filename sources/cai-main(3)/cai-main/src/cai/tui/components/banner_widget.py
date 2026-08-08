"""
Banner widget for CAI TUI - displays the CAI logo instantly with modern styling
"""

from textual.widgets import Static, RichLog
from textual.containers import Container
from textual.app import ComposeResult
import asyncio
from cai.repl.ui.banner import get_version


class BannerWidget(Container):
    """Widget that displays the CAI banner instantly with modern effects"""

    DEFAULT_CSS = """
    BannerWidget {
        height: auto;
        background: $panel;
        padding: 1 2;
        margin: 1;
        border: solid $primary-darken-1;
    }
    
    BannerWidget > RichLog {
        background: transparent;
        color: $primary;
        padding: 0;
    }
    """

    def __init__(self, **kwargs):
        super().__init__(**kwargs)
        self.banner_shown = False

    def compose(self) -> ComposeResult:
        """Compose the banner container"""
        yield RichLog(
            id="banner-output", highlight=True, markup=True, auto_scroll=False, wrap=False
        )

    async def show_banner(self, output: RichLog = None) -> None:
        """Show the CAI banner instantly"""
        if self.banner_shown:
            return

        self.banner_shown = True

        # Use provided output or find our own
        if not output:
            output = self.query_one("#banner-output", RichLog)

        version = get_version()

        # Modern CAI banner with gradient effect
        banner_lines = [
            "[bold #03fcb1]                CCCCCCCCCCCCC      ++++++++   ++++++++      IIIIIIIIII[/bold #03fcb1]",
            "[bold #03fcb1]             CCC::::::::::::C  ++++++++++       ++++++++++  I::::::::I[/bold #03fcb1]",
            "[bold #03fcb1]           CC:::::::::::::::C ++++++++++         ++++++++++ I::::::::I[/bold #03fcb1]",
            "[bold #00ff88]          C:::::CCCCCCCC::::C +++++++++    ++     +++++++++ II::::::II[/bold #00ff88]",
            "[bold #00ff88]         C:::::C       CCCCCC +++++++     +++++     +++++++   I::::I[/bold #00ff88]",
            "[bold #00ff88]        C:::::C                +++++     +++++++     +++++    I::::I[/bold #00ff88]",
            "[bold #00d9ff]        C:::::C                ++++                   ++++    I::::I[/bold #00d9ff]",
            "[bold #00d9ff]        C:::::C                 ++                     ++     I::::I[/bold #00d9ff]",
            "[bold #00d9ff]        C:::::C                  +   +++++++++++++++   +      I::::I[/bold #00d9ff]",
            "[bold #00d9ff]        C:::::C                    +++++++++++++++++++        I::::I[/bold #00d9ff]",
            "[bold #00d9ff]        C:::::C                     +++++++++++++++++         I::::I[/bold #00d9ff]",
            "[bold #00ff88]         C:::::C       CCCCCC        +++++++++++++++          I::::I[/bold #00ff88]",
            "[bold #00ff88]          C:::::CCCCCCCC::::C         +++++++++++++         II::::::II[/bold #00ff88]",
            "[bold #00ff88]           CC:::::::::::::::C           +++++++++           I::::::::I[/bold #00ff88]",
            "[bold #03fcb1]             CCC::::::::::::C             +++++             I::::::::I[/bold #03fcb1]",
            "[bold #03fcb1]                CCCCCCCCCCCCC               ++              IIIIIIIIII[/bold #03fcb1]",
            "",
            f"[bold #03fcb1 on #111111]  ◆ Cybersecurity AI (CAI), v{version} ◆  [/bold #03fcb1 on #111111]",
            "[#03fcb1]Bug bounty-ready AI framework[/#03fcb1]",
        ]

        # Display all lines instantly
        for line in banner_lines:
            output.write(line)

        output.write("")
        output.write("[#03fcb180]━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━[/#03fcb180]")
        output.write("[dim #03fcb1]💡 Type [bold]/help[/bold] for commands or chat with the AI[/dim #03fcb1]")
        output.write("[#03fcb180]━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━[/#03fcb180]")
        output.write("")

        # Force a single refresh after all content is written
        output.refresh()
        if hasattr(output, "app") and output.app:
            output.app.screen.refresh()
