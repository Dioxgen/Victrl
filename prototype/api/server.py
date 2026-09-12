"""HTTP API server for Victrl, running in a separate thread."""

import json
import logging
import threading

from flask import Flask, request, jsonify

logger = logging.getLogger("victrl.api")

app = Flask(__name__)


def create_app(agent):
    """Create and configure the Flask app with agent reference.

    Args:
        agent: VictrlAgent instance for status/control.

    Returns:
        Configured Flask app.
    """
    app.config["agent"] = agent
    app.config["lock"] = threading.Lock()
    app.config["task_thread"] = None

    @app.route("/status", methods=["GET"])
    def get_status():
        """Return current agent status."""
        a = app.config["agent"]
        with app.config["lock"]:
            status = {
                "running": a.running,
                "task_goal": a.task_goal,
                "action_count": a.action_count,
                "max_actions": a.max_actions,
                "need_screen": a.need_screen,
                "plan": a.plan_mgr.get_current_plan(),
                "history_len": len(a.short_term.history),
            }
        return jsonify(status)

    @app.route("/start", methods=["POST"])
    def start_task():
        """Start a new task. JSON body: {"task": "..."} or {"use_marker": true}."""
        a = app.config["agent"]
        data = request.get_json(silent=True) or {}

        with app.config["lock"]:
            if a.running:
                return jsonify({"error": "Agent is already running a task"}), 409

            task = data.get("task")
            if not task and data.get("use_marker"):
                task = "Marker task (placeholder)"
            if not task:
                return jsonify({"error": "Missing 'task' in request body"}), 400

            # Start task in background thread
            def _run():
                a.run(task)

            t = threading.Thread(target=_run, daemon=True)
            app.config["task_thread"] = t
            t.start()

        return jsonify({"status": "started", "task": task}), 200

    @app.route("/stop", methods=["POST"])
    def stop_task():
        """Stop the currently running task."""
        a = app.config["agent"]
        with app.config["lock"]:
            if not a.running:
                return jsonify({"error": "No task is running"}), 409
            a.stop()
        return jsonify({"status": "stopped"}), 200

    @app.route("/profile", methods=["GET"])
    def get_profile():
        """Return the current device profile content."""
        a = app.config["agent"]
        content = a.profile_mgr.load_full_text()
        return jsonify({"content": content})

    @app.route("/profile", methods=["POST"])
    def post_profile():
        """Append content to the device profile. JSON body: {"content": "..."}."""
        a = app.config["agent"]
        data = request.get_json(silent=True) or {}
        content = data.get("content", "")
        if not content.strip():
            return jsonify({"error": "Missing 'content' in request body"}), 400
        a.profile_mgr.append_content(content)
        a.profile_text = a.profile_mgr.load_full_text()
        return jsonify({"status": "updated"}), 200

    @app.route("/plan", methods=["GET"])
    def get_plan():
        """Return the current plan JSON."""
        a = app.config["agent"]
        plan = a.plan_mgr.get_current_plan()
        if plan is None:
            return jsonify({"error": "No active plan"}), 404
        return jsonify(plan)

    return app


def start_api_server(agent, host: str = "127.0.0.1", port: int = 8080):
    """Start the Flask API server in a daemon thread.

    Args:
        agent: VictrlAgent instance.
        host: Bind address.
        port: Bind port.

    Returns:
        The Flask app instance.
    """
    flask_app = create_app(agent)
    t = threading.Thread(
        target=lambda: flask_app.run(host=host, port=port, debug=False, use_reloader=False),
        daemon=True,
    )
    t.start()
    logger.info(f"API server started on {host}:{port}")
    return flask_app
