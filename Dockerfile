FROM scratch

WORKDIR /app
COPY docker-panel /app/docker-panel
COPY public ./public

ENV PORT=8080

EXPOSE 8080
ENTRYPOINT ["/app/docker-panel"]
