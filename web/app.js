// EigenScript playground host.
//
// Loads the WASM module produced by web/build.sh, wires Module.print /
// printErr into the on-page <pre id="output">, and runs the editor
// buffer when the user hits "run" (or ctrl+enter / cmd+enter).

const SOURCE = document.getElementById("source");
const OUTPUT = document.getElementById("output");
const STATUS = document.getElementById("status");
const RUN = document.getElementById("run");
const VERSION = document.getElementById("version");
const EXAMPLES = document.getElementById("examples");

const SNIPPETS = {
  hello: `# Welcome to the EigenScript playground.
# WASM interpreter-only — no JIT, no networking, no subprocess.
print of "hello, eigenscript"
`,
  defaults: `# Trailing default parameters (0.13).
define greet(name, greeting is "hi") as:
    print of (greeting + ", " + name)

greet of "world"
greet of ["bob", "hey"]
`,
  observer: `# Every assignment is observable; \`prev of\` reads the previous value.
x is 1
x is x + 10
x is x * 2
print of f"now={x}  prev={prev of x}  was={state_at of [x, 1]}"
`,
  listcomp: `# List comprehension with filter.
evens is [i for i in range of 20 if i % 2 == 0]
print of evens
print of f"sum={sum of evens}"
`,
  closure: `# Closures capture by reference; the counter survives across calls.
define make_counter() as:
    count is 0
    define tick() as:
        count is count + 1
        return count
    return tick

c is make_counter of null
print of (c of null)  # 1
print of (c of null)  # 2
print of (c of null)  # 3
`,
};

function setStatus(text, kind) {
  STATUS.textContent = text;
  STATUS.className = kind || "";
}

function appendOutput(text, isErr) {
  const span = document.createElement("span");
  if (isErr) span.className = "err";
  span.textContent = text;
  OUTPUT.appendChild(span);
  OUTPUT.scrollTop = OUTPUT.scrollHeight;
}

let runFn = null;
let versionFn = null;

EigsModule({
  print: (line) => appendOutput(line + "\n", false),
  printErr: (line) => appendOutput(line + "\n", true),
}).then((mod) => {
  runFn = mod.cwrap("eigs_run_source", "number", ["string"]);
  versionFn = mod.cwrap("eigs_version", "string", []);
  VERSION.textContent = "v" + versionFn();
  setStatus("ready", "ok");
  RUN.disabled = false;
}).catch((e) => {
  setStatus("module load failed", "error");
  appendOutput("Module load error: " + e + "\n", true);
});

function run() {
  if (!runFn) return;
  OUTPUT.textContent = "";
  setStatus("running…", "");
  RUN.disabled = true;
  // Defer one frame so the "running…" label paints before the VM blocks
  // the main thread. EigenScript runs synchronously inside the WASM
  // module; there's no yield point.
  requestAnimationFrame(() => {
    const t0 = performance.now();
    let code;
    try {
      code = runFn(SOURCE.value);
    } catch (e) {
      appendOutput("Host error: " + e + "\n", true);
      setStatus("crashed", "error");
      RUN.disabled = false;
      return;
    }
    const dt = (performance.now() - t0).toFixed(1);
    if (code === 0) {
      setStatus(`done in ${dt} ms`, "ok");
    } else {
      setStatus(`exit ${code} in ${dt} ms`, "error");
    }
    RUN.disabled = false;
  });
}

RUN.addEventListener("click", run);
SOURCE.addEventListener("keydown", (e) => {
  if ((e.ctrlKey || e.metaKey) && e.key === "Enter") {
    e.preventDefault();
    run();
  }
});

EXAMPLES.addEventListener("change", () => {
  SOURCE.value = SNIPPETS[EXAMPLES.value] || "";
});

SOURCE.value = SNIPPETS.hello;
RUN.disabled = true;
setStatus("loading wasm…", "");
