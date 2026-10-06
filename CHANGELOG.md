# Changelog

## v1.3

Both fixes are for very high frame rates (300+ fps).

- **Others see you run smoothly.** Other players no longer see you stutter-step when you walk or run.
- **Stairs and cliffs climb normally.** Climbing steps no longer pushes you back.

## v1.2

- **Full speed when locked on.** Running toward a locked-on target under certain conditions is no longer slower.
- **Effect sounds play in full.** Sounds like the casting chant no longer cut off early.

## v1.1

- **Works with xicamera in either load order.** Loading xicamera after TrueFPS no longer fails with "failed to locate
  maxDistanceSig", custom camera distances and speeds no longer switch camera eye follow to whole ticks, and TrueFPS
  unloads and reloads cleanly while xicamera is loaded, before or after xicamera itself unloads.
- **Camera tools that retune the camera** (follow, recenter, auto-rotate, spring and zoom rates) no longer turn those
  routines off: TrueFPS reads the value the game is using instead of expecting the stock one.
- **HorizonXI support.** The cast bar is now its own routine, so it is smooth on HorizonXI's older client; the hold bar,
  which that client does not have, shows as "not in this client" instead of an error. On retail, a missing hold bar is
  reported as moved by a client update.
- **Clean shutdown.** Closing the game no longer logs a false "the client had replaced its frame timer" warning.
- **Self-recovery.** A routine taken off because another tool changed its code goes back in by itself once the code is
  clean again, without switching smooth mode off and on.
- **Clearer unload messages.** The log names each routine left in and why, and only says another tool owns a camera
  value while one really does.
- **FPS Counter tab:** the Colour row is greyed while the colour threshold is on.

## v1.0

Initial release.