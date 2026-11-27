// A real CommonJS module exercising __dirname/__filename/module.exports/require.
const path = require('path');
module.exports = {
  who: 'cjs-lib',
  dir: __dirname,
  file: __filename,
  sep: path.sep,
  join: path.join('a', 'b'),
  add: (a, b) => a + b,
};
