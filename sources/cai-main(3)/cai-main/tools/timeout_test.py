#!/usr/bin/env python3
"""
Attempt to induce litellm.Timeout by sending many large concurrent requests.
"""

import warnings
warnings.filterwarnings("ignore")

import asyncio
import os
import litellm
from dotenv import load_dotenv
from rich.console import Console
from rich.panel import Panel
import time

# Load environment variables
load_dotenv()

console = Console()
litellm.suppress_debug_info = True
import logging
logging.getLogger("httpx").setLevel(logging.WARNING)
logging.getLogger("LiteLLM").setLevel(logging.WARNING)

API_BASE = "https://api.aliasrobotics.com:666/"
API_KEY = os.getenv("ALIAS_API_KEY", "").strip()
MODEL = os.getenv("CAI_MODEL", "alias1")

# Generate a large prompt to increase processing time
LARGE_PROMPT = """
Please analyze the following complex scenario and provide a detailed response:

""" + "\n".join([f"Point {i}: " + "x" * 100 for i in range(50)])

async def make_heavy_request(request_id: int, timeout: float = 5.0):
    """Make a heavy request with large prompt and short timeout."""
    start_time = time.time()
    try:
        response = await litellm.acompletion(
            model=MODEL,
            messages=[{"role": "user", "content": LARGE_PROMPT}],
            api_base=API_BASE,
            api_key=API_KEY,
            custom_llm_provider="openai",
            max_tokens=1000,  # Request many tokens
            temperature=0.7,
            timeout=timeout  # Short timeout
        )
        return {
            "id": request_id,
            "status": "success",
            "duration": time.time() - start_time
        }
    except litellm.exceptions.Timeout as e:
        console.print(f"\n[bold red]⏱️  TIMEOUT![/bold red] Request {request_id} timed out after {time.time() - start_time:.2f}s")
        console.print(f"[red]Error: {str(e)}[/red]")
        return {
            "id": request_id,
            "status": "timeout",
            "duration": time.time() - start_time,
            "error": str(e)
        }
    except litellm.exceptions.RateLimitError as e:
        return {
            "id": request_id,
            "status": "rate_limit",
            "duration": time.time() - start_time,
            "error": str(e)
        }
    except Exception as e:
        return {
            "id": request_id,
            "status": "error",
            "duration": time.time() - start_time,
            "error": str(e)[:100]
        }

async def main():
    console.print(Panel(
        "[bold cyan]Timeout Induction Test[/bold cyan]\n\n"
        f"Model: {MODEL}\n"
        f"Strategy: Large prompts + short timeouts + concurrent requests\n"
        f"Goal: Reproduce litellm.Timeout exceptions",
        title="🚀 Starting Test"
    ))
    
    # Test 1: Single request with very short timeout
    console.print("\n[yellow]Test 1: Single request with 2 second timeout...[/yellow]")
    result = await make_heavy_request(1, timeout=2.0)
    if result["status"] == "timeout":
        console.print("[green]✓ Successfully induced timeout![/green]")
    
    # Test 2: Multiple concurrent requests with short timeouts
    console.print("\n[yellow]Test 2: 20 concurrent heavy requests with 5 second timeout...[/yellow]")
    tasks = []
    for i in range(20):
        task = make_heavy_request(i + 1, timeout=5.0)
        tasks.append(task)
    
    start_time = time.time()
    results = await asyncio.gather(*tasks)
    duration = time.time() - start_time
    
    # Count results
    timeouts = sum(1 for r in results if r["status"] == "timeout")
    successes = sum(1 for r in results if r["status"] == "success")
    rate_limits = sum(1 for r in results if r["status"] == "rate_limit")
    errors = sum(1 for r in results if r["status"] == "error")
    
    console.print(f"\n[bold]Results:[/bold]")
    console.print(f"Duration: {duration:.2f}s")
    console.print(f"⏱️  Timeouts: {timeouts}")
    console.print(f"✅ Successes: {successes}")
    console.print(f"⚠️  Rate Limits: {rate_limits}")
    console.print(f"❌ Errors: {errors}")
    
    if timeouts > 0:
        console.print(Panel(
            f"[bold green]✓ Successfully reproduced litellm.Timeout![/bold green]\n\n"
            f"Got {timeouts} timeout exceptions out of {len(results)} requests.\n"
            f"This confirms we can reproduce the timeout behavior.",
            title="Timeout Reproduced",
            border_style="green"
        ))
        
        # Show a timeout error
        timeout_result = next(r for r in results if r["status"] == "timeout")
        console.print(f"\n[yellow]Timeout error example:[/yellow]")
        console.print(f"{timeout_result['error']}")
    else:
        console.print("\n[red]No timeouts induced. The infrastructure may be handling the load well.[/red]")

if __name__ == "__main__":
    asyncio.run(main())