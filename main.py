# main.py

class Router:
    def handle_request(self, path: str):
        if path == "/":
            return {"type": "text", "body": "<h1>Welcome to my server!</h1>"}
        elif path == "/file":
            return {"type": "file", "path": "index.html"}
        else:
            return {"type": "text", "body": "<h1>404 Not Found</h1>"}

router = Router()