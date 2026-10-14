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
  container.appendChild(todayEl);
  var totalEl = document.createElement('p');
  container.appendChild(totalEl);

  var link = document.createElement('a');
  link.href = '/plugin?name=reading-stats-viewer&file=index.html';
  link.textContent = 'Open full stats →';
  link.style.display = 'inline-block';
  link.style.marginTop = '8px';
  link.style.color = '#2c3e50';
  container.appendChild(link);

  function dayKey(date) {
    return date.getFullYear() + '-' + String(date.getMonth() + 1).padStart(2, '0') + '-' +
      String(date.getDate()).padStart(2, '0');
  }

  function addSession(daySeconds, start, end, duration) {
    if (!Number.isSafeInteger(end) || end <= start || duration > end - start || end - start > 7 * 86400) {
      var startDate = new Date(start * 1000);
      var startKey = dayKey(startDate);
      daySeconds[startKey] = (daySeconds[startKey] || 0) + duration;
      return;
    }
    var cursor = new Date(start * 1000);
    var endMs = end * 1000;
    var elapsed = end - start;
    var consumed = 0;
    var allocated = 0;
    while (cursor.getTime() < endMs) {
      var next = new Date(cursor);
      next.setHours(24, 0, 0, 0);
      var segmentEnd = Math.min(next.getTime(), endMs);
      var segment = Math.max(1, Math.floor((segmentEnd - cursor.getTime()) / 1000));
      var nextAllocated = Math.floor(duration * (consumed + segment) / elapsed);
      var key = dayKey(cursor);
      daySeconds[key] = (daySeconds[key] || 0) + nextAllocated - allocated;
      allocated = nextAllocated;
      consumed += segment;
      cursor = new Date(segmentEnd);
    }
    if (allocated < duration) {
      var lastKey = dayKey(new Date((end - 1) * 1000));
      daySeconds[lastKey] = (daySeconds[lastKey] || 0) + duration - allocated;
    }
  }

  fetch('/api/reading-stats')
    .then(function(response) {
      if (!response.ok) throw new Error('HTTP ' + response.status);
      return response.text();
    })
    .then(function(text) {
      var lines = text.trim().split('\n').filter(Boolean);
      var daySeconds = {};
      var total = 0;
      lines.forEach(function(line) {
        try {
          var obj = JSON.parse(line);
          var start = Number(obj && obj.s);
          var end = Number(obj && obj.e);
          var duration = Number(obj && obj.d);
          if (!obj || Array.isArray(obj) || typeof obj !== 'object' ||
              !Number.isSafeInteger(start) || !Number.isSafeInteger(end) ||
              !Number.isSafeInteger(duration) || duration <= 0 || duration > 86400) return;
          total += duration;
          addSession(daySeconds, start, end, duration);
        } catch(e) {}
      });
      var days = Object.keys(daySeconds).filter(function(key) { return daySeconds[key] > 0; }).sort();
      if (days.length === 0) {
        streakEl.textContent = 'No reading data yet.';
        todayEl.textContent = '';
        totalEl.textContent = '';
        return;
      }
      var now = new Date();
      var cursor = new Date(now);
      var todayKey = dayKey(cursor);
      var streak = 0;
      while (daySeconds[dayKey(cursor)] > 0) {
        streak++;
        cursor.setDate(cursor.getDate() - 1);
      }
      if (streak === 0) {
        cursor = new Date(now);
        cursor.setDate(cursor.getDate() - 1);
        while (daySeconds[dayKey(cursor)] > 0) {
          streak++;
          cursor.setDate(cursor.getDate() - 1);
        }
      }
      streakEl.textContent = 'Streak: ' + streak + ' days';
      todayEl.textContent = 'Today: ' + Math.floor((daySeconds[todayKey] || 0) / 60) + ' min';
      totalEl.textContent = 'Total: ' + Math.floor(total / 60) + ' min (' + days.length + ' days)';
    })
    .catch(function() {
      streakEl.textContent = 'Could not load stats.';
    });
});
