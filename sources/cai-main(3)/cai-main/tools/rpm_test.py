#!/usr/bin/env python3
"""
Properly test rate limit by checking current status and waiting if needed.
"""

import warnings
warnings.filterwarnings("ignore")

import asyncio
import os
import httpx
from dotenv import load_dotenv
from rich.console import Console
from rich.panel import Panel
import time

# Load environment variables
load_dotenv()

console = Console()

API_BASE = "https://api.aliasrobotics.com:666/"
API_KEY = os.getenv("ALIAS_API_KEY", "").strip()
MODEL = os.getenv("CAI_MODEL", "alias1")

async def check_rate_limit_status(session: httpx.AsyncClient):
    """Check current rate limit status."""
    headers = {
        "Authorization": f"Bearer {API_KEY}",
        "Content-Type": "application/json"
    }
    
    data = {
        "model": MODEL,
        "messages": [{"role": "user", "content": "Hi"}],
        "max_tokens": 10,
        "temperature": 0.7
    }
    
    try:
        response = await session.post(
            f"{API_BASE}v1/chat/completions",
            headers=headers,
            json=data,
            timeout=10.0
        )
        
        return {
            "status": response.status_code,
            "rpm_limit": int(response.headers.get("x-ratelimit-limit-requests", 60)),
            "rpm_remaining": int(response.headers.get("x-ratelimit-remaining-requests", 0)),
            "tpm_limit": int(response.headers.get("x-ratelimit-limit-tokens", 500000)),
            "tpm_remaining": int(response.headers.get("x-ratelimit-remaining-tokens", 0))
        }
    except httpx.HTTPStatusError as e:
        if e.response.status_code == 429:
            return {
                "status": 429,
                "rpm_limit": 60,
                "rpm_remaining": 0,
                "error": "Currently rate limited"
            }
        raise

async def make_request(session: httpx.AsyncClient, request_id: int):
    """Make a single request."""
    headers = {
        "Authorization": f"Bearer {API_KEY}",
        "Content-Type": "application/json"
    }
    
    data = {
        "model": MODEL,
        "messages": [{"role": "user", "content": "Hi"}],
        "max_tokens": 10,
        "temperature": 0.7
    }
    
    try:
        response = await session.post(
            f"{API_BASE}v1/chat/completions",
            headers=headers,
            json=data,
            timeout=10.0
        )
        
        return {
            "id": request_id,
            "status": response.status_code,
            "rpm_remaining": response.headers.get("x-ratelimit-remaining-requests", "?")
        }
    except httpx.HTTPStatusError as e:
        return {
            "id": request_id,
            "status": e.response.status_code,
            "rpm_remaining": e.response.headers.get("x-ratelimit-remaining-requests", "?")
        }

async def main():
    console.print(Panel(
        "[bold cyan]Rate Limit Test with Proper Status Check[/bold cyan]\n\n"
        "This script will check the current rate limit status and test accordingly.",
        title="🚀 Starting Test"
    ))
    
    async with httpx.AsyncClient() as session:
        # First check current rate limit status
        console.print("\n[yellow]Checking current rate limit status...[/yellow]")
        status = await check_rate_limit_status(session)
        
        if status["status"] == 429:
            console.print("[red]Currently rate limited! Please wait a minute and try again.[/red]")
            return
        
        console.print(f"RPM Limit: {status['rpm_limit']}")
        console.print(f"RPM Remaining: {status['rpm_remaining']}")
        
        if status['rpm_remaining'] < 65:
            console.print(f"\n[yellow]Only {status['rpm_remaining']} requests remaining in current window.[/yellow]")
            console.print("[yellow]Waiting 60 seconds for rate limit to reset...[/yellow]")
            await asyncio.sleep(60)
            
            # Check again
            status = await check_rate_limit_status(session)
            console.print(f"\nAfter waiting - RPM Remaining: {status['rpm_remaining']}")
        
        # Now send 65 requests to exceed the 60 limit
        console.print(f"\n[bold green]Sending 65 requests to exceed the {status['rpm_limit']} RPM limit...[/bold green]\n")
        
        tasks = []
        for i in range(65):
            tasks.append(make_request(session, i + 1))
        
        start_time = time.time()
        results = await asyncio.gather(*tasks)
        duration = time.time() - start_time
        
        # Count results
        success_200 = sum(1 for r in results if r["status"] == 200)
        rate_limited_429 = sum(1 for r in results if r["status"] == 429)
        
        console.print(f"\n[bold]Results:[/bold]")
        console.print(f"Duration: {duration:.2f}s")
        console.print(f"✅ Successful (200): {success_200}")
        console.print(f"⚠️  Rate Limited (429): {rate_limited_429}")
        
        # Show some individual results
        console.print(f"\n[bold]Sample Results:[/bold]")
        for i in [0, 30, 58, 59, 60, 61, 62, 63, 64]:
            if i < len(results):
                r = results[i]
                status_str = "[green]200[/green]" if r["status"] == 200 else "[red]429[/red]"
                console.print(f"Request {r['id']:2d}: Status {status_str}, RPM Remaining: {r['rpm_remaining']}")
        
        if rate_limited_429 > 0:
            console.print(Panel(
                f"[bold green]✓ Rate limiting confirmed![/bold green]\n\n"
                f"Successfully sent {success_200} requests before hitting the rate limit.\n"
                f"The remaining {rate_limited_429} requests were rate limited with 429 status.",
                title="Test Successful",
                border_style="green"
            ))

if __name__ == "__main__":
    asyncio.run(main())