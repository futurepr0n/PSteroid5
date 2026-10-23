// Dev-only socket.io fixture for PSteroids5's native client tests.
// Prints the port, then: 'echo' -> 'echo' (same payload), 'big' -> 300 KB string,
// 'bye' -> server disconnects the socket. Short ping interval exercises pong handling.
const { createServer } = require('http');
const { Server } = require('socket.io');
const http = createServer();
const io = new Server(http, { pingInterval: 300, pingTimeout: 1500 });
io.on('connection', socket => {
  socket.emit('welcome', { sid: socket.id, n: 1 });
  socket.on('echo', data => socket.emit('echo', data));
  socket.on('big', () => socket.emit('big', 'x'.repeat(300 * 1024)));
  socket.on('bye', () => socket.disconnect(true));
});
http.listen(Number(process.argv[2] || 0), process.argv[3] || '127.0.0.1', () => console.log(http.address().port));
