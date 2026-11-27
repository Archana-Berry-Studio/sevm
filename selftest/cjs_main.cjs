// CJS module that requires another CJS module + a builtin (sync).
const lib = require('./cjs_lib.cjs');
const os = require('os');
module.exports = {
  fromMain: 'yes',
  lib,
  osPlatform: os.platform(),
  dir: __dirname,
  file: __filename,
};
