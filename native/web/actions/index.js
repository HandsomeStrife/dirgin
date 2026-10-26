// Action registry for play.js. Each action is its own module in this folder that
// exports createAction() (an action per ACTIONS.md) and HELP (a short HUD hint, e.g.
// 'Space jump'). Modules that are missing or fail to load are skipped with a warning,
// so actions can be added or removed independently.
const NAMES = ['reload', 'jump', 'ladder', 'shoot'];

export async function loadActions() {
  const out = [];
  for (const name of NAMES) {
    try {
      const m = await import(`./${name}.js`);
      if (typeof m.createAction !== 'function') { console.warn(`actions/${name}.js: no createAction()`); continue; }
      out.push({ name, action: m.createAction(), help: m.HELP || '' });
    } catch (e) {
      if (name !== 'reload') console.info(`actions/${name}.js not loaded: ${e.message}`);
      else console.warn(e);
    }
  }
  return out;
}
