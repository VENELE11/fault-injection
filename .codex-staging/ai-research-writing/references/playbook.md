# AI Research Writing Playbook

This reference keeps the output formats compact and consistent with the
`awesome-ai-research-writing` workflow.

## Output shapes

### 1. Chinese to English LaTeX

Default output:

- `Part 1 [LaTeX]`: polished English text only
- `Part 2 [Translation]`: Chinese back-translation for checking meaning

Checklist:

- no unexplained Chinese left in the English output
- LaTeX special characters escaped when needed
- equations preserved
- no decorative formatting unless requested

### 2. English to Chinese explanation

Default output:

- translated Chinese text only, unless the user wants notes

Checklist:

- remove `\cite{}`, `\ref{}`, `\label{}`
- keep sentence order close to the original
- explain formulas in readable language when helpful
- do not add polishing that changes the original stance

### 3. Academic polishing

Default output:

- revised passage
- optional short note on the main improvements if the user seems to want them

Checklist:

- grammar and style cleaned
- simple academic vocabulary preferred
- no contractions in English
- avoid awkward possessives for model or method names when possible

### 4. Logic check

Default output:

- `Major issues`
- `Minor issues`
- `Quick fixes`

Focus on:

- claim-evidence mismatch
- fairness of baselines
- ablation coverage
- clarity of contribution
- consistency between intro, method, and experiments

### 5. De-AI / humanize

Default output:

- rewritten text only
- optional short note on what was toned down

Patterns to remove:

- empty significance claims
- repetitive three-part rhetoric
- generic transitions
- vague attribution
- dash-heavy cadence

### 6. Figure and table text

Default output:

- title candidates or caption candidates

Checklist:

- mention task, setting, or comparison axis
- mention metric names when known
- keep concise enough for paper layout

### 7. Experiment analysis

Default output:

- `Observations`
- `Interpretation`
- `Potential reviewer concerns`

Guardrails:

- do not overclaim causality
- separate evidence from hypothesis
- note when results are noisy or incomplete

### 8. Reviewer-style audit

Default output:

- `Part 1 [The Review Report]`
- `Summary`
- `Strengths`
- `Weaknesses (Critical)`
- `Rating`
- `Part 2 [Strategic Advice]`

Guardrails:

- be honest about strengths
- make weaknesses concrete and fix-oriented
- distinguish structural flaws from rebuttal-fixable issues

## Venue-sensitive tone

If the user names venues such as NeurIPS, ICML, ICLR, ACL, AAAI, or COLM:

- use restrained, review-friendly academic tone
- favor crisp claims and explicit evidence
- avoid marketing language

## When repo context is available

If the user gives a code repo or experiment folder:

- anchor the writing to actual files and results
- draft only what the artifacts support
- mark unsupported details as assumptions instead of inventing them
