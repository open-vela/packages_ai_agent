# Device Self Check

Check the board's network and multimedia peripherals, then report a concise
pass/fail summary.

## When to use
When the user asks whether the camera, microphone, speaker, display, I2S, or
network is ready.

## How to use
1. Call `peripheral_status`.
2. Call `network_probe` with `apply=false`.
3. Report every missing device node and all reachable DNS servers.
4. If the speaker is available and the user requested an audible result,
   call `tts_speak` with the summary.
5. Never call `network_probe` with `apply=true` unless the user explicitly
   asks to change DNS.

## Result
Separate hardware presence from end-to-end functional verification. A
present device node means registered, not that image or audio quality has
been verified.
