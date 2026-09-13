"""Cross-language interoperability oracle, using the pinned official MCP SDK."""
import importlib.metadata
import sys
from pathlib import Path
from mcp.server.fastmcp import Context, FastMCP
from mcp.types import ToolAnnotations

assert importlib.metadata.version("mcp") == "1.26.0", "Use the documented MCP oracle version"
secret_file = Path(sys.argv[1])
server = FastMCP("iiLocalLLM-official-oracle", log_level="WARNING")


@server.tool(annotations=ToolAnnotations(readOnlyHint=True))
async def read_secret(ctx: Context) -> dict[str, str]:
    """Read the exact secret value from the fixture file. No arguments are needed."""
    await ctx.report_progress(1, 2, "Reading fixture")
    value = secret_file.read_text()
    await ctx.report_progress(2, 2, "Read complete")
    return {"value": value}


@server.tool(annotations=ToolAnnotations(readOnlyHint=True))
async def inspect_roots(ctx: Context) -> dict[str, int]:
    """Ask the client for the explicitly shared roots."""
    result = await ctx.session.list_roots()
    return {"count": len(result.roots)}


@server.resource("fixture://secret")
def secret_resource() -> str:
    return secret_file.read_text()


@server.prompt()
def summarize(topic: str) -> str:
    return "Summarize " + topic


server.run(transport="stdio")
