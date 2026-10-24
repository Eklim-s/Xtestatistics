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

  function validAggregate(value) {
    if (!value || Array.isArray(value) || typeof value !== 'object') return {};
    return ['seconds', 'days', 'record', 'current', 'last'].every(function(key) {
      return Number.isSafeInteger(value[key]) && value[key] >= 0;
    }) && value.current <= value.record && value.record <= value.days ? value : {};
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
      var contribution = nextAllocated - allocated;
      if (contribution > 0) daySeconds[key] = (daySeconds[key] || 0) + contribution;
      allocated = nextAllocated;
      consumed += segment;
      cursor = new Date(segmentEnd);
    }
    if (allocated < duration) {
      var lastKey = dayKey(new Date((end - 1) * 1000));
      daySeconds[lastKey] = (daySeconds[lastKey] || 0) + duration - allocated;
    }
  }

  Promise.all([fetch('/api/reading-stats'), fetch('/api/reading-stats-summary')])
    .then(function(responses) {
      responses.forEach(function(response) {
        if (!response.ok) throw new Error('HTTP ' + response.status);
      });
      return Promise.all([responses[0].text(), responses[1].json()]);
    })
    .then(function(payload) {
      var text = payload[0];
      var aggregate = validAggregate(payload[1]);
      var lines = text.trim().split('\n').filter(Boolean);
      var daySeconds = Object.create(null);
      var total = 0;
      lines.forEach(function(line) {
        try {
          var obj = JSON.parse(line);
          var start = Number(obj && obj.s);
          var end = Number(obj && obj.e);
          var duration = Number(obj && obj.d);
          if (!obj || Array.isArray(obj) || typeof obj !== 'object' ||
               !Number.isSafeInteger(start) || start <= 0 || start > 253402300799 ||
               !Number.isSafeInteger(end) || end < 0 || end > 253402300799 ||
              !Number.isSafeInteger(duration) || duration <= 0 || duration > 86400) return;
          total += duration;
          addSession(daySeconds, start, end, duration);
        } catch(e) {}
      });
      var days = Object.keys(daySeconds).filter(function(key) { return daySeconds[key] > 0; }).sort();
      if (days.length === 0 && !Number.isSafeInteger(Number(aggregate.seconds))) {
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
      if (Number.isSafeInteger(aggregate.last)) {
        var yesterday = new Date(now);
        yesterday.setDate(yesterday.getDate() - 1);
        var today = Number.isSafeInteger(aggregate.today) ? aggregate.today : Number(dayKey(now).replace(/-/g, ''));
        var previous = Number.isSafeInteger(aggregate.yesterday) ? aggregate.yesterday : Number(dayKey(yesterday).replace(/-/g, ''));
        streak = aggregate.last === today || aggregate.last === previous ? aggregate.current : 0;
      }
      streakEl.textContent = 'Device streak: ' + streak + ' days';
      todayEl.textContent = 'Today: ' + Math.floor((daySeconds[todayKey] || 0) / 60) + ' min';
      var aggregateTotal = Number.isSafeInteger(Number(aggregate.seconds)) ? Number(aggregate.seconds) : total;
      var aggregateDays = Number.isSafeInteger(Number(aggregate.days)) ? Number(aggregate.days) : days.length;
      totalEl.textContent = 'Total: ' + Math.floor(aggregateTotal / 60) + ' min (' + aggregateDays + ' days)';
    })
    .catch(function() {
      streakEl.textContent = 'Could not load stats.';
    });
});
