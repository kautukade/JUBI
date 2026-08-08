"""
Tool for executing code via LLM tool calls.
"""

import os

from cai.tools.common import run_command  # pylint: disable=import-error
from cai.sdk.agents import function_tool


def _resolve_execute_code_output_dir(output_directory: str | None) -> tuple[str | None, str | None]:
    """Return (cwd for run_command, None) or (None, error). Creates directory if missing."""
    if output_directory is None or not str(output_directory).strip():
        return None, None
    path = os.path.abspath(os.path.expanduser(str(output_directory).strip()))
    try:
        os.makedirs(path, exist_ok=True)
    except OSError as exc:
        return None, f"Error: cannot create output_directory {path}: {exc}"
    if not os.path.isdir(path):
        return None, f"Error: output_directory is not a directory: {path}"
    return path, None


@function_tool
def execute_code(
    code: str = "",
    language: str = "python",
    filename: str = "exploit",
    timeout: int = 100,
    ctf=None,
    output_directory: str | None = None,
) -> str:
    """
    Create a file code store it and execute it

    This tool allows for executing code provided in different
    programming languages. It creates a permanent file with the provided code
    and executes it using the appropriate interpreter. You can exec this
code as many times as you want using `generic_linux_command` tool.

    Priorize: Python and Perl

    Args:
        code: The code snippet to execute
        language: Programming language to use (default: python)
        filename: Base name for the file without extension (default: exploit)
        timeout: Timeout for the execution (default: 100 seconds)
                Use high timeout for long running code
                Use low timeout for short running code
        output_directory: Optional directory where the source file is created and commands run
                (local host). Use when the user specifies an absolute folder for artifacts.
                The directory is created if it does not exist.

    Returns:
        Command output or error message from execution
    """

    if not code:
        return "No code provided to execute"

    cwd_pass, cwd_err = _resolve_execute_code_output_dir(output_directory)
    if cwd_err:
        return cwd_err

    # Map file extensions
    extensions = {
        "python": "py",
        "php": "php",
        "bash": "sh",
        "shell": "sh",  # Add shell as alias for bash
        "ruby": "rb",
        "perl": "pl",
        "golang": "go",
        "go": "go",  # Add go as alias for golang
        "javascript": "js",
        "js": "js",  # Add js as alias for javascript
        "typescript": "ts",
        "ts": "ts",  # Add ts as alias for typescript
        "rust": "rs",
        "csharp": "cs",
        "cs": "cs",  # Add cs as alias for csharp
        "java": "java",
        "kotlin": "kt",
        "c": "c",  # Add C language
        "cpp": "cpp",  # Add C++ language
        "c++": "cpp",  # Add C++ language alias
    }
    # Normalize language to lowercase
    language = language.lower()
    ext = extensions.get(language, "txt")
    full_filename = f"{filename}.{ext}"

    # Create code file with content
    create_cmd = f"cat << 'EOF' > {full_filename}\n{code}\nEOF"
    # Don't stream the file creation and suppress output display
    result = run_command(
        create_cmd, ctf=ctf, stream=False, tool_name="_internal_file_creation", workspace_dir=cwd_pass
    )
    if "error" in result.lower():
        return f"Failed to create code file: {result}"

    # Prepare execution command based on language
    if language in ["python", "py"]:
        exec_cmd = f"python3 {full_filename}"
    elif language in ["php"]:
        exec_cmd = f"php {full_filename}"
    elif language in ["bash", "sh", "shell"]:
        exec_cmd = f"bash {full_filename}"
    elif language in ["ruby", "rb"]:
        exec_cmd = f"ruby {full_filename}"
    elif language in ["perl", "pl"]:
        exec_cmd = f"perl {full_filename}"
    elif language in ["golang", "go"]:
        temp_dir = f"/tmp/go_exec_{filename}"
        run_command(
            f"mkdir -p {temp_dir}", ctf=ctf, stream=False, tool_name="_internal_setup", workspace_dir=cwd_pass
        )
        run_command(
            f"cp {full_filename} {temp_dir}/main.go",
            ctf=ctf,
            stream=False,
            tool_name="_internal_setup",
            workspace_dir=cwd_pass,
        )
        run_command(
            f"cd {temp_dir} && go mod init temp",
            ctf=ctf,
            stream=False,
            tool_name="_internal_setup",
            workspace_dir=cwd_pass,
        )
        exec_cmd = f"cd {temp_dir} && go run main.go"
    elif language in ["javascript", "js"]:
        exec_cmd = f"node {full_filename}"
    elif language in ["typescript", "ts"]:
        exec_cmd = f"ts-node {full_filename}"
    elif language in ["rust", "rs"]:
        # For Rust, we need to compile first
        run_command(
            f"rustc {full_filename} -o {filename}",
            ctf=ctf,
            stream=False,
            tool_name="_internal_setup",
            workspace_dir=cwd_pass,
        )
        exec_cmd = f"./{filename}"
    elif language in ["csharp", "cs"]:
        # For C#, compile with dotnet
        run_command(
            f"dotnet build {full_filename}", ctf=ctf, stream=False, tool_name="_internal_setup", workspace_dir=cwd_pass
        )
        exec_cmd = f"dotnet run {full_filename}"
    elif language in ["java"]:
        # For Java, compile first
        run_command(
            f"javac {full_filename}", ctf=ctf, stream=False, tool_name="_internal_setup", workspace_dir=cwd_pass
        )
        exec_cmd = f"java {filename}"
    elif language in ["kotlin", "kt"]:
        # For Kotlin, compile first
        run_command(
            f"kotlinc {full_filename} -include-runtime -d {filename}.jar",
            ctf=ctf,
            stream=False,
            tool_name="_internal_setup",
            workspace_dir=cwd_pass,
        )
        exec_cmd = f"java -jar {filename}.jar"
    elif language in ["c"]:
        # For C, compile with gcc
        run_command(
            f"gcc {full_filename} -o {filename}", ctf=ctf, stream=False, tool_name="_internal_setup", workspace_dir=cwd_pass
        )
        exec_cmd = f"./{filename}"
    elif language in ["cpp", "c++"]:
        # For C++, compile with g++
        run_command(
            f"g++ {full_filename} -o {filename}", ctf=ctf, stream=False, tool_name="_internal_setup", workspace_dir=cwd_pass
        )
        exec_cmd = f"./{filename}"
    else:
        return f"Unsupported language: {language}"

    # Execute the code with syntax-highlighted output
    # Create a custom tool args dictionary to send language and code info to the tool output function
    tool_args = {
        "command": "execute",
        "language": language,
        "filename": filename,
        "code": code,  # Include the code for syntax highlighting
        "timeout": timeout,
    }

    # Run the command with streaming to get syntax highlighting
    output = run_command(
        exec_cmd,
        ctf=ctf,
        timeout=timeout,
        stream=True,  # ALWAYS use streaming
        tool_name="execute_code",
        args=tool_args,
        workspace_dir=cwd_pass,
    )

    return output


# --- Auto-register with ToolRegistry ---
from cai.tool_registry import TOOL_REGISTRY  # noqa: E402
TOOL_REGISTRY.register("execute_code", execute_code, categories=["recon", "exploitation", "misc"])
