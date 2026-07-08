#pragma once
#include "../warvk_bridge.j"

library WarVKLightning
    function WarVK_LightningCreate takes real x0, real y0, real z0, real x1, real y1, real z1 returns integer
        return WarVK_QueryInteger("lightning-create|" + R2S(x0) + "|" + R2S(y0) + "|" + R2S(z0) + "|" + R2S(x1) + "|" + R2S(y1) + "|" + R2S(z1))
    endfunction

    function WarVK_LightningMove takes integer id, real x0, real y0, real z0, real x1, real y1, real z1 returns boolean
        return WarVK_QueryInteger("lightning-move|" + I2S(id) + "|" + R2S(x0) + "|" + R2S(y0) + "|" + R2S(z0) + "|" + R2S(x1) + "|" + R2S(y1) + "|" + R2S(z1)) == 1
    endfunction

    function WarVK_LightningDestroy takes integer id returns boolean
        return WarVK_QueryInteger("lightning-destroy|" + I2S(id)) == 1
    endfunction

    function WarVK_LightningSetColor takes integer id, real r0, real g0, real b0, real a0, real r1, real g1, real b1, real a1 returns boolean
        return WarVK_QueryInteger("lightning-set-color|" + I2S(id) + "|" + R2S(r0) + "|" + R2S(g0) + "|" + R2S(b0) + "|" + R2S(a0) + "|" + R2S(r1) + "|" + R2S(g1) + "|" + R2S(b1) + "|" + R2S(a1)) == 1
    endfunction

    function WarVK_LightningSetWidth takes integer id, real startWidth, real endWidth returns boolean
        return WarVK_QueryInteger("lightning-set-width|" + I2S(id) + "|" + R2S(startWidth) + "|" + R2S(endWidth)) == 1
    endfunction

    function WarVK_LightningSetCurve takes integer id, real curveAmplitude, real noiseAmplitude, integer segments, integer branchCount returns boolean
        return WarVK_QueryInteger("lightning-set-curve|" + I2S(id) + "|" + R2S(curveAmplitude) + "|" + R2S(noiseAmplitude) + "|" + I2S(segments) + "|" + I2S(branchCount)) == 1
    endfunction

    function WarVK_LightningSetLifetime takes integer id, real lifetimeSec, real fadeInSec, real fadeOutSec returns boolean
        return WarVK_QueryInteger("lightning-set-lifetime|" + I2S(id) + "|" + R2S(lifetimeSec) + "|" + R2S(fadeInSec) + "|" + R2S(fadeOutSec)) == 1
    endfunction

    function WarVK_LightningSetPulse takes integer id, real amplitude, real frequencyHz returns boolean
        return WarVK_QueryInteger("lightning-set-pulse|" + I2S(id) + "|" + R2S(amplitude) + "|" + R2S(frequencyHz)) == 1
    endfunction

    function WarVK_LightningGetActiveCount takes nothing returns integer
        return WarVK_QueryInteger("lightning-active-count")
    endfunction

    function WarVK_LightningGetStats takes nothing returns string
        return WarVK_QueryString("lightning-stats")
    endfunction
endlibrary
