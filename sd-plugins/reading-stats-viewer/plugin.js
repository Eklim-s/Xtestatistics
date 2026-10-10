// Reading Stats Viewer — summary card on the Settings page
CrossPoint.registerPlugin(function(container, api) {
  container.style.maxWidth = '420px';
  container.style.margin = '0 auto';

  var h2 = document.createElement('h2');
  h2.textContent = 'Reading Stats';
  container.appendChild(h2);

  var streakEl = document.createElement('p');
  streakEl.style.fontSize = '18px';
  streakEl.style.fontWeight = 'bold';
  streakEl.textContent = 'Loading…';
  container.appendChild(streakEl);

  var todayEl = document.createElement('p');
  todayEl.textContent = '';
  container.appendChild(todayEl);

  var totalEl = document.createElement('p');
  totalEl.textContent = '';
  container.appendChild(totalEl);

  var link = document.createElement('a');
  link.href = '/plugin?name=reading-stats-viewer&file=index.html';
  link.textContent = 'Open full stats →';
  link.style.display = 'inline-block';
  link.style.marginTop = '8px';
  link.style.color = '#2c3e50';
  container.appendChild(link);

  // Fetch stats from the device API.
  fetch('/api/reading-stats')
    .then(function(r) { return r.text(); })
    .then(function(text) {
      var lines = text.trim().split('\n').filter(Boolean);
      var dayMinutes = {};
      var total = 0;
      var now = Math.floor(Date.now() / 1000);

      lines.forEach(function(line) {
        try {
          var obj = JSON.parse(line);
          var dur = parseInt(obj.d) || 0;
          total += dur;
          // Group by day (UTC date from epoch)
          var date = new Date(parseInt(obj.s) * 1000);
          var key = date.getUTCFullYear() + '-' +
                    String(date.getUTCMonth() + 1).padStart(2, '0') + '-' +
                    String(date.getUTCDate()).padStart(2, '0');
          dayMinutes[key] = (dayMinutes[key] || 0) + Math.round(dur / 60);
        } catch(e) {}
      });

      var totalMin = Math.round(total / 60);
      var days = Object.keys(dayMinutes).sort();

      // Streak
      var yesterday = new Date((now - 86400) * 1000);
      var streak = 0;
      var d = new Date(yesterday);
      while (true) {
        var key = d.getUTCFullYear() + '-' +
                  String(d.getUTCMonth() + 1).padStart(2, '0') + '-' +
                  String(d.getUTCDate()).padStart(2, '0');
        if (!dayMinutes[key] || dayMinutes[key] < 1) break;
        streak++;
        d.setUTCDate(d.getUTCDate() - 1);
      }

      var todayKey = new Date(now * 1000).toISOString().slice(0, 10);
      var today = dayMinutes[todayKey] || 0;

      streakEl.textContent = 'Streak: ' + streak + ' days';
      todayEl.textContent = 'Today: ' + today + ' min';
      totalEl.textContent = 'Total: ' + totalMin + ' min (' + days.length + ' days)';
    })
    .catch(function(err) {
      streakEl.textContent = 'Could not load stats.';
    });
});