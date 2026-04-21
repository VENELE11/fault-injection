---
name: ai-research-writing
description: Use when the user wants help with academic paper writing or revision based on the awesome-ai-research-writing playbook: translating Chinese and English research text, polishing LaTeX or Word prose, reducing AI-written tone, checking logic, generating figure or table titles, analyzing experiments, or reviewing a paper from a reviewer perspective.
---

# AI Research Writing

This skill adapts the workflow and writing habits from
[Leey21/awesome-ai-research-writing](https://github.com/Leey21/awesome-ai-research-writing)
into a Codex-friendly playbook.

Use it for CS and AI paper drafting, rewriting, polishing, and review support.
When the user provides a PDF or DOCX, combine this skill with the local `pdf` or
`doc` skill for file handling, then apply the writing guidance here.

## Core defaults

- Keep claims faithful to the evidence in the draft, repo, tables, or figures.
- Prefer simple, common academic vocabulary over flashy wording.
- Avoid hype, vague significance claims, repetitive three-part phrasing, and em
  dashes unless the user explicitly wants them.
- Preserve equations, symbols, and technical terms.
- For LaTeX output, keep math intact and escape special characters such as `%`,
  `_`, and `&`.
- Prefer coherent paragraphs over bullet-heavy prose unless the user asks for a
  checklist or review report.
- Default to present tense for methods, setup, and conclusions. Use past tense
  only for clearly time-bound events or procedures.
- If the user names a target venue, aim for top-tier ML/AI conference tone.

## Pick the right mode

Choose the narrowest mode that fits the request:

1. Chinese draft to English paper text
2. English LaTeX to Chinese reading aid
3. English or Chinese polishing
4. Logic check
5. De-AI / humanize
6. Figure or table naming and captioning
7. Experiment analysis
8. Reviewer-style paper audit
9. Drafting sections from a repo, notes, or results

If the user asks for several tasks at once, do them in sequence and keep the
output clearly segmented.

## Mode guidance

### Chinese draft to English paper text

- Translate into natural academic English rather than literal classroom English.
- Keep the output LaTeX-safe when the source is LaTeX or the user mentions LaTeX.
- Unless the user asks otherwise, return:
  - `Part 1 [LaTeX]`: the polished English text only
  - `Part 2 [Translation]`: a Chinese back-translation for meaning checks

### English LaTeX to Chinese reading aid

- Remove distracting commands such as `\cite{}`, `\ref{}`, and `\label{}`.
- Translate the content inside formatting commands but ignore the formatting.
- Convert formulas into readable Chinese explanations when that improves clarity.
- Stay structurally close to the original so the user can map the Chinese back
  to the English source.

### English or Chinese polishing

- Fix grammar, spelling, punctuation, and article usage.
- Improve clarity, flow, and formal tone without changing the technical meaning.
- Avoid contractions in English academic prose.
- Prefer `the performance of METHOD` over `METHOD's performance` when that makes
  the sentence feel more conference-ready.

### Logic check

Look for:

- claims not supported by experiments
- missing or unfair baselines
- missing ablations for key design choices
- novelty claims that are not defended clearly
- mismatch between introduction promises and experimental evidence
- vague causal wording that should be narrowed

### De-AI / humanize

Remove patterns often associated with AI-written text:

- inflated importance claims
- salesy or generic framing
- repetitive sentence rhythm
- vague attributions such as "it is widely known"
- overused em dashes
- empty summary sentences that add no technical value

Keep the original meaning and the author's likely intent.

### Figure or table naming and captioning

- Make titles and captions specific, not generic.
- Highlight the comparison axis, dataset, metric, or setting when known.
- Mention metric direction (`higher is better`, `lower is better`) when useful.
- Keep captions publication-ready and compact.

### Experiment analysis

- Interpret the results conservatively.
- Separate observations from explanations.
- Call out the strongest and weakest evidence.
- Point out likely reviewer questions if the claims seem ahead of the data.

### Reviewer-style paper audit

When the user wants a paper-level critique, structure the answer as:

- `Part 1 [The Review Report]`
- `Summary`
- `Strengths`
- `Weaknesses (Critical)`
- `Rating`
- `Part 2 [Strategic Advice]`

Be specific and actionable. Distinguish fatal issues from fixable issues.

### Drafting from repo, notes, or results

- Start from concrete artifacts: repo path, README, experiment logs, tables,
  captions, notes, or target venue.
- Infer a plausible contribution story, but label uncertainty instead of
  inventing evidence.
- Draft in the natural order for the task. For a full paper, a good default is:
  Abstract, Introduction, Method, Experiments, Related Work, Limitations.

## Input collection

Ask only for missing information that materially changes the answer:

- target venue
- file format: LaTeX, Markdown, or Word
- section name
- repo or results path
- whether bilingual checking output is wanted

If the user does not provide these, make a reasonable assumption and state it
briefly after the output.

## Reference

For compact templates and mode-specific output shapes, read
`references/playbook.md`.
