// Action registry for play.js. Each action is its own module in this folder that
// exports createAction() (an action per ACTIONS.md) and HELP (a short HUD hint, e.g.
// 'Space ladder'). Modules that are missing or fail to load are skipped with a warning,
// so actions can be added or removed independently.
// Only the ladder is on the page. Removed by worker 0078 (invented, not translations): the
// jump (a later job translates FUN_00575768's physics-body branch), shooting and reload
// (worker 0077's aiming/weapon translation, FUN_00579570, will replace them).
const NAMES = ['ladder'];

export async function loadActions() {
  const out = [];
  for (const name of NAMES) {
    try {
      const m = await import(`./${name}.js`);
      if (typeof m.createAction !== 'function') { console.warn(`actions/${name}.js: no createAction()`); continue; }
      out.push({ name, action: m.createAction(), help: m.HELP || '' });
    } catch (e) {
      console.warn(`actions/${name}.js not loaded: ${e.message}`);
    }
  }
  return out;
}
