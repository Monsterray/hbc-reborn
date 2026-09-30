# The wii-bench lease server: which workstation has the bench Wii now.
#   docker compose up -d --build        (from this directory)
FROM python:3.12-alpine
COPY wiibench.py /app/wiibench.py
EXPOSE 4310
CMD ["python", "-u", "/app/wiibench.py", "serve", "--port", "4310"]
