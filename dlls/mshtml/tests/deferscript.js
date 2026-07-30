defer_count++;
defer_order.push("deferred-" + defer_count);
ok(document.getElementById("defer-target") !== null,
   "deferred script executed before its following body content was parsed");

if(defer_count === 1) {
    document.addEventListener("DOMContentLoaded", function() {
        deferred_dom_content_loaded = true;
        defer_order.push("DOMContentLoaded");
    });
}
