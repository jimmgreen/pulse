// Original caller fixture: no hook awareness, same execFile shape as DeepSeek.
const {execFile} = require('node:child_process');
const fs = require('node:fs');
const cases = JSON.parse(fs.readFileSync(process.argv[2], 'utf8').replace(/^\uFEFF/, ''));
(async () => {
  for (const item of cases) {
    const start = performance.now();
    let callbacks = 0;
    await new Promise(resolve => {
      const child = execFile(item.command, item.args, {
        timeout: 5000, windowsHide: false, encoding: 'utf8', cwd: item.cwd,
        env: {...process.env, PROBE_CHILD_ENV: 'preserved'},
      }, (error, stdout, stderr) => {
        callbacks++;
        setImmediate(() => {
          console.log(JSON.stringify({name: item.name, pid: child.pid,
            callbacks, code: error ? error.code : 0, killed: !!error?.killed,
            stdout, stderr, elapsed_ms: +(performance.now()-start).toFixed(3)}));
          resolve();
        });
      });
    });
  }
})().catch(error => { console.error(error); process.exitCode = 1; });
