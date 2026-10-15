# Product design principles

- Design complete user journeys. A feature includes discovery, setup, execution, feedback, recovery, and a clear successful outcome; implement the whole experience.
- Make the product do the work. Detect what can be detected, automate routine steps, and prevent predictable failures. Ask users only for decisions or information the product cannot determine.
- Make the current state clear. Show what the product knows, what it is doing, and what happens next. Respond immediately to user actions with meaningful progress, and resolve waiting states when conditions change.
- Keep each workflow coherent. Present related states and actions in the relevant screen. Prefer inline, nonblocking messages over interrupting dialogs, preserve the user's context, and remove obsolete or repeated instructions.
- Make recovery actionable. Explain the issue in plain language and provide a complete next step, including copyable commands when needed. Technical details support diagnosis; they must not be required to understand or resolve an ordinary problem.
- Own the recommendation. Verify compatibility and behavior before recommending an action. Use concise, confident product language grounded in evidence; do not substitute blame, user attribution, or defensive qualification for engineering work.
- Design screens as composed pages. Use a clear reading order, consistent spacing, bounded content widths, and appropriately sized controls. Integrate setup, progress, and recovery into the page's hierarchy, with an obvious primary action and expandable supporting details.
- Keep appearance consistent across the application. Use one shared stylesheet and palette; preserve the application's dark mode. Screens inherit theme colors, including highlights, rather than defining their own themes. Simplification must retain the information needed to understand the current state and the result of an action.
- Treat Linux support as an end-to-end product commitment, including installation, permissions, hardware discovery, updates, and recovery. Handle environment requirements as part of the experience.
- Verify the experience in the delivered build. Exercise successful flows and common failure states, inspect the rendered UI, and document remaining work as concrete product tasks.

# Workspace practices

- Make standard file edits directly and visibly. Do not hide edits in scripts or temporary patch files.
- Never run sudo or another privilege-elevation mechanism. Give the user the exact command to run when root is required.
- Never commit or push unless explicitly requested in the current request; each action needs its own instruction.
- When the user refers to a screenshot, inspect the newest relevant image in ~/Screenshots.
- After presenting a UI prompt that asks for a response, stop and wait for the user's answer.
