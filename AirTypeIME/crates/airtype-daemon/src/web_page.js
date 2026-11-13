// 设置页面的设备表轮询。整份脚本零依赖，由 web.rs 用 include_str! 内嵌。
// 设备名来自 START 帧（也就是来自网络），所以任何拼接进 HTML 的字段都必须过 esc()。
(function () {
  "use strict";

  var tbody = document.getElementById("device-rows");
  if (!tbody) {
    return;
  }

  function esc(value) {
    var s = value == null ? "" : String(value);
    return s.replace(/[&<>"']/g, function (c) {
      return {
        "&": "&amp;",
        "<": "&lt;",
        ">": "&gt;",
        '"': "&quot;",
        "'": "&#39;"
      }[c];
    });
  }

  // 延迟用 ms 显示，空闲时长用 s 显示（超过 1s 才有可读性）。
  function fmtMs(v) {
    if (!v) {
      return "-";
    }
    return v < 1000 ? v + " ms" : (v / 1000).toFixed(1) + " s";
  }

  function render(devices) {
    if (!devices.length) {
      tbody.innerHTML =
        '<tr><td colspan="7" class="muted">暂无设备连接</td></tr>';
      return;
    }
    var rows = devices.map(function (d) {
      var status = d.connected
        ? '<span class="on">在线</span>'
        : '<span class="off">离线 ' + fmtMs(d.idle_ms) + "</span>";
      var latency = d.utterances
        ? d.last_first_partial_ms + " ms"
        : "-";
      var total = d.utterances ? d.last_total_ms + " ms" : "-";
      return (
        "<tr><td>" + esc(d.device) + "</td>" +
        "<td>" + esc(d.peer) + "</td>" +
        "<td>" + status + "</td>" +
        "<td>" + d.utterances + "</td>" +
        "<td>" + latency + "</td>" +
        "<td>" + total + "</td>" +
        '<td class="muted">' + esc(d.last_text) + "</td></tr>"
      );
    });
    tbody.innerHTML = rows.join("");
  }

  function refresh() {
    fetch("/api/state", { cache: "no-store" })
      .then(function (r) {
        return r.ok ? r.json() : null;
      })
      .then(function (state) {
        if (state && state.devices) {
          render(state.devices);
        }
      })
      .catch(function () {
        // daemon 正在重启或页面短暂不可达，下一轮再试即可。
      });
  }

  refresh();
  setInterval(refresh, 3000);
})();
