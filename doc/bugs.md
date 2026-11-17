## Bugs/incompatibilities found:

- Windows server + plugin on the same machine FM8 FX fails with `cannot resolve resource: application_ENG`
- Same for Reaktor6 FX, and it makes the server crash
- Plugdata : any change in the patch induces a HUGE CPU spike on the server.
- Plugdata : a message in an inlet of audio objects has no effect (e.g. a number into the inlet of [*~ 1] doesn't change the value)
