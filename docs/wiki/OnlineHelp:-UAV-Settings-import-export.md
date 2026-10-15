# Controller settings import and export

UAV settings are the flight controller's object settings. They are separate from the application's desktop layout and preferences.

Export the controller's settings before installing firmware or making changes you may need to reverse. Use the settings import/export actions in the application to save or open a settings file.

When importing, review the summary and choose which objects to save to the controller. Check the result for each selected object. An import can contain objects or fields that do not match the connected controller's firmware.

After an import:

1. Resolve failed or incompatible objects shown in the summary.
2. Restart the controller when the imported hardware or module settings require it.
3. Recheck vehicle type, receiver inputs, motor assignments, output protocols, sensor orientation and arming behavior before flight.

For desktop preferences and workspace layouts, use [Application settings import and export](OnlineHelp:-Import-Export).

Replaces the [dRonin controller-settings import/export placeholder](https://github.com/d-ronin/dRonin/wiki/OnlineHelp:-UAV-Settings-import-export).
