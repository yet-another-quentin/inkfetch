FROM python:3.12-slim

WORKDIR /app
COPY pyproject.toml ./
COPY src ./src
RUN pip install --no-cache-dir . && useradd --system inkfetch

USER inkfetch
EXPOSE 8080
ENTRYPOINT ["inkfetch"]
CMD ["serve", "--help"]
