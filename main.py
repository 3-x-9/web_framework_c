import myframework

app = myframework.Server()

@app.get("/")
def index():
    return {"type": "text", "body": "<h1>Welcome to myframework!</h1>"}


@app.get("/hello")
def hello():    
    return {"type": "text", "body": "<h1>Hello, World!</h1>"}

@app.get("/user/{id}")
def user(id):
    return {"type": "text", "body": f"<h1>User: {id}</h1>"}

@app.get("/file")
def serve_file():
    return {"type": "file", "path": "index.html"}


if __name__ == "__main__":
    app.run()
