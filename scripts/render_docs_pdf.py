from __future__ import annotations

from pathlib import Path
from urllib.parse import urlparse

import markdown
from bs4 import BeautifulSoup
from playwright.sync_api import sync_playwright


ROOT = Path(__file__).resolve().parent.parent
DOCS = [
    (ROOT / "README.md", ROOT / "README.pdf"),
    (ROOT / "clients" / "web_ui" / "README.md", ROOT / "clients" / "web_ui" / "README.pdf"),
    (ROOT / "ACTION_PLAN.md", ROOT / "ACTION_PLAN.pdf"),
]

CSS = """
body { font-family: 'Segoe UI', Arial, sans-serif; line-height: 1.32; margin: 10mm; color: #20252b; font-size: 10.2pt; }
h1, h2, h3, h4 { color: #17212b; margin-top: 0.75em; margin-bottom: 0.35em; }
h1 { font-size: 19pt; } h2 { font-size: 14.5pt; } h3 { font-size: 12pt; } h4 { font-size: 11pt; }
p, li { margin-top: 0.25em; margin-bottom: 0.25em; }
ul, ol { margin-top: 0.25em; margin-bottom: 0.35em; padding-left: 1.2em; }
pre { background: #f3f5f7; padding: 8px 10px; border-radius: 6px; overflow-x: auto; white-space: pre-wrap; font-size: 8.8pt; }
code { background: #f3f5f7; padding: 1px 4px; border-radius: 3px; }
img { max-width: 100%; height: auto; display: block; }
table { border-collapse: collapse; width: 100%; margin: 0.45em 0; table-layout: fixed; }
th, td { border: 1px solid #d7dde3; padding: 5px 6px; text-align: left; vertical-align: top; font-size: 9.2pt; }
th { background: #eef2f6; }
sub { font-size: 8.1pt; line-height: 1.15; }
hr { border: none; border-top: 1px solid #d9dde2; margin: 0.8em 0; }
"""


def _is_external(url: str) -> bool:
    parsed = urlparse(url)
    return parsed.scheme in {"http", "https", "data", "file"}


def _rewrite_local_images(html: str, base_dir: Path) -> str:
    soup = BeautifulSoup(html, "html.parser")
    for tag in soup.find_all("img"):
        src_attr = tag.get("src")
        src = src_attr if isinstance(src_attr, str) else None
        if not src or _is_external(src):
            continue
        img_path = (base_dir / src).resolve()
        tag["src"] = img_path.as_uri()
    return str(soup)


def _render_markdown(md_path: Path) -> str:
    raw = md_path.read_text(encoding="utf-8")
    html = markdown.markdown(raw, extensions=["tables", "fenced_code", "toc"])
    html = _rewrite_local_images(html, md_path.parent)
    return (
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        f"<style>{CSS}</style></head><body>{html}</body></html>"
    )


def render_pdf(md_path: Path, pdf_path: Path, browser) -> None:
    html_path = pdf_path.with_suffix(".render.html")
    html_path.write_text(_render_markdown(md_path), encoding="utf-8")

    page = browser.new_page(viewport={"width": 1280, "height": 1800})
    page.goto(html_path.resolve().as_uri(), wait_until="load")
    page.wait_for_load_state("networkidle")
    page.wait_for_timeout(1200)
    page.pdf(
        path=str(pdf_path),
        format="A4",
        print_background=True,
        margin={"top": "10mm", "bottom": "10mm", "left": "10mm", "right": "10mm"},
    )
    page.close()
    html_path.unlink(missing_ok=True)


def main() -> None:
    with sync_playwright() as p:
        browser = p.chromium.launch()
        for md_path, pdf_path in DOCS:
            render_pdf(md_path, pdf_path, browser)
        browser.close()


if __name__ == "__main__":
    main()