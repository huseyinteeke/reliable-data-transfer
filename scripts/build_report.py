#!/usr/bin/env python3
"""Builds report/report.pdf from report/report.md.

Needs:  pip install markdown weasyprint matplotlib
Mermaid blocks are replaced by state-machine figures drawn here with matplotlib.
"""
import re
from pathlib import Path

import markdown
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import FancyBboxPatch
from weasyprint import HTML

ROOT = Path(__file__).resolve().parent.parent
FIG = ROOT / "plot"


def state_figure(path, title, states, arrows, loops):
    """states: [(name, x)], arrows: [(i, j, label)], loops: {i: [text,...]}"""
    fig, ax = plt.subplots(figsize=(11, 3.6))
    ax.set_xlim(-0.8, 3.3 * (len(states) - 1) + 1.3)
    ax.set_ylim(-3.0, 1.4)
    ax.axis("off")
    ax.set_title(title, fontsize=11)
    for i, (name, x) in enumerate(states):
        ax.add_patch(FancyBboxPatch((x - 0.65, -0.3), 1.3, 0.6, boxstyle="round,pad=0.05",
                                    fc="#e8f1f5", ec="#00798c", lw=1.5))
        ax.text(x, 0, name, ha="center", va="center", fontsize=10, weight="bold")
        for j, t in enumerate(loops.get(i, [])):
            ax.text(x - 0.7, -0.65 - 0.42 * j, "↻ " + t, ha="left", va="center", fontsize=7.5)
    for i, j, label in arrows:
        x0, x1 = states[i][1] + 0.7, states[j][1] - 0.7
        ax.annotate("", xy=(x1, 0), xytext=(x0, 0),
                    arrowprops=dict(arrowstyle="-|>", color="#333", lw=1.3))
        ax.text((x0 + x1) / 2, 0.12, label, ha="center", va="bottom", fontsize=7.5)
    fig.tight_layout()
    fig.savefig(path, dpi=150)
    plt.close(fig)


def make_figures():
    state_figure(
        FIG / "fsm_sender.png", "Sender state machine",
        [("SENDING", 0), ("FIN_WAIT", 3.3), ("DONE", 6.6)],
        [(0, 1, "base == N\n(all DATA acked)\nsend FIN"), (1, 2, "FIN_ACK\n(number == N mod M)")],
        {0: ["send new packet while next − base < W",
             "ACK advances base (GBN: cumulative; SR: mark + slide)",
             "timeout: GBN resend window / SR resend packet; RTO × 2",
             "bad CRC / stale / duplicate ACK: ignore"],
         1: ["timeout: resend FIN; RTO × 2"]})
    state_figure(
        FIG / "fsm_receiver.png", "Receiver state machine",
        [("RECEIVING", 0), ("DONE", 3.3), ("EXIT", 6.6)],
        [(0, 1, "FIN, number ==\nexpected mod M\nsend FIN_ACK, close file"), (1, 2, "idle for\n`linger` ms")],
        {0: ["valid DATA: deliver / buffer / re-ACK (rules of section 1.2)",
             "bad CRC / length / range: drop silently",
             "FIN with other number: ignore (data still missing)"],
         1: ["FIN again: send FIN_ACK again"]})


CSS = """
@page { size: A4; margin: 18mm 16mm; @bottom-center { content: counter(page); font-size: 9pt; color: #666; } }
body { font-family: 'DejaVu Sans', sans-serif; font-size: 9.5pt; line-height: 1.4; color: #111; }
h1 { font-size: 19pt; border-bottom: 2px solid #00798c; padding-bottom: 4px; }
h2 { font-size: 14pt; margin-top: 18pt; color: #00596a; border-bottom: 1px solid #ccc; }
h3 { font-size: 11pt; color: #00596a; }
table { border-collapse: collapse; margin: 8pt 0; font-size: 8pt; width: 100%; }
th, td { border: 1px solid #bbb; padding: 3px 5px; vertical-align: top; }
th { background: #e8f1f5; }
code { font-family: 'DejaVu Sans Mono', monospace; font-size: 8.3pt; background: #f2f2f2; padding: 0 2px; }
pre { background: #f6f6f6; padding: 6px; font-size: 8pt; overflow-wrap: anywhere; white-space: pre-wrap; }
img { max-width: 100%; display: block; margin: 8pt auto; }
hr { border: 0; border-top: 1px solid #ddd; }
"""


def main():
    make_figures()
    md = (ROOT / "report" / "report.md").read_text()
    figs = iter(["fsm_sender.png", "fsm_receiver.png"])
    md = re.sub(r"```mermaid.*?```", lambda m: f"![state machine](../plot/{next(figs)})", md, flags=re.S)
    html_body = markdown.markdown(md, extensions=["tables", "fenced_code", "sane_lists"])
    html = f"<html><head><meta charset='utf-8'><style>{CSS}</style></head><body>{html_body}</body></html>"
    out = ROOT / "report" / "report.pdf"
    HTML(string=html, base_url=str(ROOT / "report") + "/").write_pdf(out)
    print("wrote", out.relative_to(ROOT))


if __name__ == "__main__":
    main()
