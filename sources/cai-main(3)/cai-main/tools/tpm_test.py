#!/usr/bin/env python3
"""
Script to saturate the Tokens Per Minute (TPM) limit of the LLM endpoint.
Target: 490,000 TPM (just below the 500,000 TPM limit)
"""

# Suppress warnings before any imports
import warnings
warnings.filterwarnings("ignore", message=".*UnsupportedFieldAttributeWarning.*")
warnings.filterwarnings("ignore", category=UserWarning, module="pydantic")

import asyncio
import os
import time
from typing import List, Dict, Any
import litellm
import logging
from datetime import datetime
import tiktoken
from dotenv import load_dotenv
from rich.console import Console
from rich.table import Table
from rich.panel import Panel
from rich.progress import Progress, SpinnerColumn, TextColumn, BarColumn, TimeRemainingColumn
from rich.live import Live
from rich import print as rprint

# Load environment variables from .env file
dotenv_path = os.path.join(os.getcwd(), '.env')
load_dotenv(dotenv_path=dotenv_path, verbose=False)

# Configure logging - set to WARNING to reduce noise
logging.basicConfig(
    level=logging.WARNING,
    format='%(asctime)s - %(levelname)s - %(message)s'
)
logger = logging.getLogger(__name__)

# Suppress HTTP request logs from httpx
logging.getLogger("httpx").setLevel(logging.WARNING)
logging.getLogger("httpcore").setLevel(logging.WARNING)

# Rich console
console = Console()

# Suppress debug info from litellm
litellm.suppress_debug_info = True
litellm.set_verbose = False

# Disable litellm logging
logging.getLogger("LiteLLM").setLevel(logging.WARNING)
logging.getLogger("litellm").setLevel(logging.WARNING)

# Configuration
TARGET_TPM = 490000  # Target tokens per minute
TARGET_TOKENS_PER_REQUEST = 8000  # Large request to maximize token usage
REQUESTS_PER_MINUTE = TARGET_TPM // TARGET_TOKENS_PER_REQUEST  # About 61 requests
REQUEST_INTERVAL = 60.0 / REQUESTS_PER_MINUTE if REQUESTS_PER_MINUTE > 0 else 1.0

# API Configuration
API_BASE = "https://api.aliasrobotics.com:666/"
API_KEY = os.getenv("ALIAS_API_KEY", "").strip()

if not API_KEY:
    raise ValueError("ALIAS_API_KEY environment variable must be set")

# Model configuration - use CAI_MODEL if set, otherwise default to alias1 
# since it's optimized and can handle large contexts
MODEL = os.getenv("CAI_MODEL", "alias1")

# Temperature configuration
TEMPERATURE = float(os.getenv("CAI_TEMPERATURE", "0.7"))

def count_tokens(text: str) -> int:
    """Count tokens in text using tiktoken."""
    try:
        encoding = tiktoken.get_encoding("cl100k_base")
    except:
        encoding = tiktoken.get_encoding("gpt2")
    return len(encoding.encode(text))

def generate_large_prompt(target_tokens: int) -> str:
    """Generate a prompt with approximately the target number of tokens."""
    # Base context about a complex technical system
    base_prompt = """You are analyzing a complex distributed system with the following characteristics:

The system consists of multiple microservices deployed across different regions:
- Frontend service: Handles user requests and UI rendering
- API Gateway: Routes requests to appropriate backend services
- Authentication service: Manages user authentication and authorization
- Database cluster: Distributed PostgreSQL with read replicas
- Cache layer: Redis cluster for session management and caching
- Message queue: RabbitMQ for asynchronous processing
- Analytics engine: Real-time data processing with Apache Spark
- Monitoring stack: Prometheus, Grafana, and custom alerting

Each service has specific performance requirements and SLAs:
1. Frontend must respond within 200ms for 95% of requests
2. API Gateway must handle 10,000 requests per second
3. Database queries must complete within 100ms
4. Cache hit ratio must be above 85%
5. Message queue processing latency must be under 500ms

The system experiences the following load patterns:
- Peak hours: 8AM-10AM and 6PM-9PM local time
- Weekend traffic is 60% of weekday traffic
- Monthly spikes on the 1st and 15th (payroll processing)
- Seasonal variations during holidays and sales events

Recent incidents and their root causes:
"""
    
    # Add detailed incident descriptions to reach target tokens
    incident_template = """
Incident #{num}: Database connection pool exhaustion
Date: 2024-{month:02d}-{day:02d}
Duration: {duration} minutes
Impact: {impact}% of users affected
Root cause: A deployment introduced a database connection leak in the payment service. Each request was creating a new connection without properly closing it. The connection pool limit of 100 was reached within 45 minutes of deployment.
Resolution: Rolled back the deployment and implemented proper connection management using try-with-resources blocks. Added monitoring for connection pool metrics.
Lessons learned: Need better testing of resource management in staging environment. Implement automatic circuit breakers for database connections.

"""
    
    current_tokens = count_tokens(base_prompt)
    incidents = []
    incident_num = 1
    
    # Generate incidents until we reach approximately the target token count
    while current_tokens < target_tokens - 500:  # Leave some buffer
        incident = incident_template.format(
            num=incident_num,
            month=(incident_num % 12) + 1,
            day=(incident_num % 28) + 1,
            duration=30 + (incident_num % 90),
            impact=5 + (incident_num % 40)
        )
        incidents.append(incident)
        current_tokens = count_tokens(base_prompt + "".join(incidents))
        incident_num += 1
    
    full_prompt = base_prompt + "".join(incidents)
    full_prompt += "\n\nBased on these incidents and system characteristics, provide a brief summary of the most critical issue."
    
    return full_prompt

async def make_large_token_request(request_id: int, prompt: str) -> Dict[str, Any]:
    """Make a single API request with large token count."""
    start_time = time.time()
    prompt_tokens = count_tokens(prompt)
    
    try:
        response = await litellm.acompletion(
            model=MODEL,
            messages=[{"role": "user", "content": prompt}],
            api_base=API_BASE,
            api_key=API_KEY,
            custom_llm_provider="openai",
            max_tokens=1000,  # Allow reasonable response
            temperature=TEMPERATURE,
            timeout=120.0  # Longer timeout for large requests
        )
        
        end_time = time.time()
        duration = end_time - start_time
        
        # Extract token usage
        usage = response.usage if hasattr(response, 'usage') else {}
        actual_prompt_tokens = getattr(usage, 'prompt_tokens', prompt_tokens)
        output_tokens = getattr(usage, 'completion_tokens', 0)
        total_tokens = getattr(usage, 'total_tokens', actual_prompt_tokens + output_tokens)
        
        return {
            "request_id": request_id,
            "status": "success",
            "duration": duration,
            "timestamp": datetime.now().isoformat(),
            "prompt_tokens_estimated": prompt_tokens,
            "input_tokens": actual_prompt_tokens,
            "output_tokens": output_tokens,
            "total_tokens": total_tokens
        }
        
    except litellm.exceptions.Timeout as e:
        console.print(f"[bold red]⏱️  TIMEOUT DETECTED![/bold red] Request {request_id} timed out after {duration:.2f}s")
        console.print(f"[red]Error: {str(e)}[/red]")
        return {
            "request_id": request_id,
            "status": "timeout",
            "duration": time.time() - start_time,
            "timestamp": datetime.now().isoformat(),
            "prompt_tokens_estimated": prompt_tokens,
            "error": str(e)
        }
    except Exception as e:
        if "rate limit" in str(e).lower():
            console.print(f"[bold yellow]⚠️  RATE LIMIT HIT![/bold yellow] Request {request_id}")
            console.print(f"[yellow]Error: {str(e)}[/yellow]")
        else:
            console.print(f"[red]❌ Request {request_id} failed: {str(e)[:100]}...[/red]", highlight=False)
        return {
            "request_id": request_id,
            "status": "error",
            "duration": time.time() - start_time,
            "timestamp": datetime.now().isoformat(),
            "prompt_tokens_estimated": prompt_tokens,
            "error": str(e)
        }

async def token_saturation_test(num_minutes: float = 2):
    """Execute token saturation test for the specified number of minutes."""
    # Display test configuration
    config_table = Table(title="TPM Saturation Test Configuration", show_header=True, header_style="bold magenta")
    config_table.add_column("Parameter", style="cyan")
    config_table.add_column("Value", style="green")
    config_table.add_row("Target TPM", f"{TARGET_TPM:,}")
    config_table.add_row("Target Tokens/Request", f"{TARGET_TOKENS_PER_REQUEST:,}")
    config_table.add_row("Requests Per Minute", str(REQUESTS_PER_MINUTE))
    config_table.add_row("Duration", f"{num_minutes} minutes")
    config_table.add_row("Model", MODEL)
    
    console.print(config_table)
    
    # Generate the large prompt once
    console.print("\n[yellow]Generating large prompt...[/yellow]")
    large_prompt = generate_large_prompt(TARGET_TOKENS_PER_REQUEST)
    actual_prompt_tokens = count_tokens(large_prompt)
    console.print(f"[green]✓ Generated prompt with {actual_prompt_tokens:,} tokens[/green]")
    
    results = []
    start_time = time.time()
    
    # Track tokens per minute window
    minute_windows = {}
    
    # Calculate total requests needed
    total_requests = int(REQUESTS_PER_MINUTE * num_minutes)
    
    # Variable to track if we should stop due to timeout
    should_stop = False
    
    # Create tasks for concurrent execution (batch by minute)
    for minute in range(int(num_minutes)):
        if should_stop:
            break
            
        minute_start = time.time()
        minute_tasks = []
        
        # Limit concurrent requests to avoid overwhelming the API
        requests_this_minute = min(50, REQUESTS_PER_MINUTE, total_requests - (minute * REQUESTS_PER_MINUTE))
        
        for i in range(requests_this_minute):
            request_id = minute * REQUESTS_PER_MINUTE + i + 1
            task = make_large_token_request(request_id, large_prompt)
            minute_tasks.append(task)
        
        # Execute requests in batches
        console.print(f"\n[cyan]Minute {minute + 1}: Sending {len(minute_tasks)} requests...[/cyan]")
        batch_size = 10
        minute_results = []
        
        with Progress(
            SpinnerColumn(),
            TextColumn("[progress.description]{task.description}"),
            BarColumn(),
            TextColumn("[progress.percentage]{task.percentage:>3.0f}%"),
            console=console,
            transient=True
        ) as progress:
            task_id = progress.add_task("[green]Processing requests...", total=len(minute_tasks))
            
            for batch_start in range(0, len(minute_tasks), batch_size):
                batch_end = min(batch_start + batch_size, len(minute_tasks))
                batch = minute_tasks[batch_start:batch_end]
                batch_results = await asyncio.gather(*batch, return_exceptions=True)
                minute_results.extend(batch_results)
                
                progress.update(task_id, advance=len(batch))
                
                # Check for timeouts
                for result in batch_results:
                    if isinstance(result, dict) and result.get("status") == "timeout":
                        should_stop = True
                        console.print("\n[bold red]🛑 Timeout detected! Stopping test...[/bold red]")
                        break
                
                if should_stop:
                    break
                    
                # Small delay between batches
                if batch_end < len(minute_tasks):
                    await asyncio.sleep(0.5)
        
        # Process results
        for result in minute_results:
            if isinstance(result, Exception):
                logger.error(f"Task exception: {result}")
                results.append({
                    "status": "error",
                    "error": str(result),
                    "timestamp": datetime.now().isoformat()
                })
            else:
                results.append(result)
                
                # Track tokens in this minute window
                if minute not in minute_windows:
                    minute_windows[minute] = {"requests": 0, "tokens": 0}
                minute_windows[minute]["requests"] += 1
                minute_windows[minute]["tokens"] += result.get("total_tokens", 0)
        
        # Wait for the remainder of the minute if needed
        minute_elapsed = time.time() - minute_start
        if minute_elapsed < 60 and minute < num_minutes - 1:
            wait_time = 60 - minute_elapsed
            logger.info(f"Waiting {wait_time:.1f}s until next minute...")
            await asyncio.sleep(wait_time)
    
    # Final statistics
    total_time = time.time() - start_time
    successful_requests = sum(1 for r in results if r.get("status") == "success")
    timeout_requests = sum(1 for r in results if r.get("status") == "timeout")
    error_requests = sum(1 for r in results if r.get("status") == "error")
    
    # Calculate total tokens used
    total_tokens = sum(r.get("total_tokens", 0) for r in results if r.get("status") == "success")
    total_estimated_tokens = sum(r.get("prompt_tokens_estimated", 0) for r in results)
    
    # Create results table
    results_table = Table(title="\n🏁 TPM Saturation Test Results", show_header=True, header_style="bold cyan")
    results_table.add_column("Metric", style="yellow")
    results_table.add_column("Value", style="white")
    
    results_table.add_row("Total Time", f"{total_time:.2f} seconds")
    results_table.add_row("Total Requests", str(len(results)))
    results_table.add_row("Successful Requests", f"[green]{successful_requests}[/green]")
    results_table.add_row("Timeout Requests", f"[red]{timeout_requests}[/red]" if timeout_requests > 0 else str(timeout_requests))
    results_table.add_row("Error Requests", f"[yellow]{error_requests}[/yellow]" if error_requests > 0 else str(error_requests))
    results_table.add_row("Total Tokens Used", f"[bold]{total_tokens:,}[/bold]")
    results_table.add_row("Estimated Tokens Sent", f"{total_estimated_tokens:,}")
    results_table.add_row("Actual TPM", f"[bold green]{(total_tokens / (total_time / 60)):,.0f}[/bold green]")
    if successful_requests > 0:
        results_table.add_row("Avg Tokens/Request", f"{(total_tokens / successful_requests):,.0f}")
    
    console.print(results_table)
    
    # Show tokens per minute window
    if minute_windows:
        window_table = Table(title="\nTokens Per Minute Window", show_header=True)
        window_table.add_column("Minute", style="cyan")
        window_table.add_column("Requests", style="green")
        window_table.add_column("Tokens", style="magenta")
        for window, stats in sorted(minute_windows.items()):
            window_table.add_row(str(window + 1), str(stats['requests']), f"{stats['tokens']:,}")
        console.print(window_table)
    
    # Check if we hit rate limits
    rate_limit_errors = [r for r in results if "rate" in str(r.get("error", "")).lower()]
    if rate_limit_errors:
        console.print(Panel(
            f"[bold yellow]⚠️  Hit rate limit {len(rate_limit_errors)} times![/bold yellow]\n\n"
            f"Rate limit errors:\n",
            title="Rate Limit Detected",
            border_style="yellow"
        ))
        for err in rate_limit_errors[:3]:  # Show first 3
            console.print(f"[yellow]  - {err.get('error', 'Unknown error')}[/yellow]")
    
    # Check if we hit timeouts
    if timeout_requests > 0:
        timeout_errors = [r for r in results if r.get("status") == "timeout"]
        achieved_tpm = (total_tokens / (total_time / 60)) if total_time > 0 else 0
        console.print(Panel(
            f"[bold red]⏱️  Hit timeout {timeout_requests} times![/bold red]\n\n"
            f"This indicates the API endpoint is saturated and cannot respond in time.\n"
            f"Achieved approximately [bold]{achieved_tpm:,.0f} TPM[/bold] before timeout.\n"
            f"The endpoint was successfully saturated!",
            title="Timeout Analysis",
            border_style="red"
        ))

async def main():
    """Main function to run the TPM saturation test."""
    console.print(Panel(
        f"[bold cyan]TPM Limit Saturation Test[/bold cyan]\n\n"
        f"[yellow]Model:[/yellow] {MODEL}\n"
        f"[yellow]Target:[/yellow] {TARGET_TPM:,} tokens per minute\n"
        f"[yellow]API:[/yellow] {API_BASE}",
        title="🚀 Test Starting",
        border_style="blue"
    ))
    
    # Check if tiktoken is available
    try:
        import tiktoken
    except ImportError:
        console.print("[red]⚠️  tiktoken not installed. Install with: pip install tiktoken[/red]")
        return
    
    try:
        # Run for 2 minutes to properly test the rate limit
        await token_saturation_test(num_minutes=2)
    except KeyboardInterrupt:
        console.print("\n[yellow]Test interrupted by user[/yellow]")
    except Exception as e:
        console.print(f"\n[red]Test failed with error: {str(e)}[/red]")
        raise

if __name__ == "__main__":
    asyncio.run(main())