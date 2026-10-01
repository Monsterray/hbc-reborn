# The wii-bench lease server: which workstation has the bench Wii now, and the history.
#   docker compose up -d --build        (from this directory)
# The history lives in /data: compose.yaml mounts ./history there. KEEP_MONTHS=0 keeps the
# monthly archives forever.
FROM python:3.12-alpine
COPY wiibench.py /app/wiibench.py
ENV KEEP_MONTHS=0
VOLUME /data
EXPOSE 4310
CMD ["sh", "-c", "exec python -u /app/wiibench.py serve --port 4310 --history /data --keep-months \"$KEEP_MONTHS\""]
