# Point-shadow receiver-bias integration

## Branch finding

The user-verified Type0/UBirth baseline and the physically verified point-shadow
bias fix were sibling branches from `f6529c6`. The Type0 branch therefore did
not contain the receiver-bias shader even though both results had separately
passed visual testing.

## Integrated correction

This checkpoint ports only the visual correction from `778491d` onto the Type0
correctness line:

- trustworthy receiver planes use one exact radial-depth domain for every PCF
  tap;
- a failed exact-plane tap fails soft instead of switching that single tap back
  to centre depth;
- surfaces without a reliable plane use a bounded, monotonic slope fallback.

This prevents one 16-tap kernel from mixing incompatible depth references, the
source of the coherent point-shadow bands reported on terrain and units.

The optional debug visualization and route counters from the sibling commit are
not required for the visual fix and are not pulled across with unrelated branch
history. No CSM, Type0, Arena, public JAPI or point-shadow scheduling policy is
changed.
