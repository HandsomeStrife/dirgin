// Node loader hook for the unit tests: stubs the three.js import (player.js only needs a few names).
import { register } from 'node:module';
register('data:text/javascript,' + encodeURIComponent(`
export async function resolve(spec, ctx, next) {
  if (spec === 'three') return { url: 'data:text/javascript,' + encodeURIComponent('export class AnimationMixer{}; export const LoopRepeat=2201, LoopOnce=2200; export class Vector3{};'), shortCircuit: true };
  return next(spec, ctx);
}`));
