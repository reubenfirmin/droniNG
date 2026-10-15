# Output configuration

Use **Configuration → Output** to select output protocols, set channel limits and verify actuator assignments.

![Output configuration](images/output.png)

**Remove all propellers before calibrating or testing motor outputs.** Output testing sends commands directly to actuators.

1. Assign motors and control surfaces on [Vehicle](OnlineHelp:-Vehicle-configuration), and gimbal outputs on [Camera Stab](OnlineHelp:-Camera-Stabilization-Configuration).
2. Select a protocol and refresh rate supported by the connected ESCs or servos. Outputs can share a timer bank, so a bank's protocol/rate can affect several channels.
3. Set the minimum, neutral and maximum for each output using the actuator's specifications. Numeric neutral fields allow precise setup without entering output test mode.
4. Click **Save**.
5. If testing is needed, enable output testing deliberately. Check one channel at a time and disable testing afterward. Leaving this configuration page also stops testing.

| Output type | Setup |
| --- | --- |
| PWM motor ESC | Set pulse limits and a supported bank rate; calibrate only if required by the ESC. |
| PWM servo | Set travel and neutral without mechanical binding, using the servo's supported pulse range and update rate. |
| OneShot / MultiShot | Use matching protocol and pulse limits; do not reuse ordinary PWM values. |
| DShot | Select a supported DShot mode; conventional PWM endpoint calibration is not used. |

There is no single safe refresh rate or endpoint range for every servo or ESC. See [ESC refresh rates](OnlineHelp:-ESC-Refresh-Rate) and [Saving changes](Saving-changes).

Adapted from the [dRonin output help](https://github.com/d-ronin/dRonin/wiki/OnlineHelp:-Output-Configuration). The old model-specific defaults have been replaced with protocol and device compatibility guidance.
