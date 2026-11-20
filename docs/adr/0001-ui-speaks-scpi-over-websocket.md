# The web UI controls the instrument with SCPI over WebSocket

The browser sends the same SCPI text commands that scripts send over TCP, through the same parser and command tree, and gets its own Session. We chose this over a separate JSON control API so there is exactly one path for changing instrument state: the UI and scripts can never disagree, and every UI control exercises the command tree as a free integration test. The cost is that the UI must build SCPI strings and read SCPI replies instead of typed JSON.
