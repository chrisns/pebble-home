var dev = require('./dev_config');
require('./hass').init({
  name: 'face',
  devUrl: dev.url,
  devRemote: dev.remote,
  devKey: dev.key,
  pins: true,
  demo: dev.demo,
  legacyConfigUrl: 'data:text/html;charset=utf-8,' + encodeURIComponent(require('./config_page'))
});
