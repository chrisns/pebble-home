var dev = require('./dev_config');
require('./hass').init({
  name: 'app',
  devUrl: dev.url,
  devRemote: dev.remote,
  devKey: dev.key,
  pins: false, // the face pins events; one pin per event
  replayFresh: true, // opened during a doorbell: show it again
  demo: dev.demo,
  legacyConfigUrl: 'data:text/html;charset=utf-8,' + encodeURIComponent(require('./config_page'))
});
