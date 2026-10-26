// Example action: standing reload on F, the h_st_rd clip (vincent_shoot.glb pack).
// Proves the action interface (player.js header / ACTIONS.md): enter returns an
// animation request, update owns movement (he stands still), and the clip's onEnd
// callback ends the action. Only the body animates: the gun's own reload motion
// (07770580 h_st_rd, gun bone 3 turns ~82 deg) isn't exported yet.
export const KEY = 'KeyF';
const CLIP = 'h_st_rd';

export function reloadAction() {
  let ended = false;
  return {
    name: 'reload',
    canEnter(ctx) {
      return ctx.input.pressed?.has(KEY) && ctx.ctrl.onGround && ctx.anim?.has(CLIP);
    },
    enter(ctx) {
      ended = false;
      return { clip: CLIP, loop: false, rate: 1, fade: 0.15, onEnd: () => { ended = true; } };
    },
    update(dt, ctx) {
      // stand still (no delta); done once the clip has played out
      return { done: ended };
    },
    exit(ctx) {
      ctx.anim.release(0.2);
    },
  };
}

export const HELP = 'F reload';
export function createAction() { return reloadAction(); }
