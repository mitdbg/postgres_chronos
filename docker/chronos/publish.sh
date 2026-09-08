#!/usr/bin/env bash
set -Eeuo pipefail

repo_root=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
account_home=$(getent passwd "$(id -u)" | cut -d: -f6)
docker_config_dir=${DOCKER_CONFIG:-$account_home/.docker}
registry_auth=$(jq -er '.auths["https://index.docker.io/v1/"].auth' "$docker_config_dir/config.json" | base64 --decode)
logged_in_user=${registry_auth%%:*}
dockerhub_namespace=${DOCKERHUB_NAMESPACE:-$logged_in_user}
dockerhub_repository=${DOCKERHUB_REPOSITORY:-chronos-postgres}
dockerhub_private=${DOCKERHUB_PRIVATE:-false}
revision=$(git -C "$repo_root" rev-parse HEAD)
short_revision=${revision:0:12}
major_image="$dockerhub_namespace/$dockerhub_repository:19"
version_image="$dockerhub_namespace/$dockerhub_repository:19beta3"
revision_image="$dockerhub_namespace/$dockerhub_repository:19beta3-$short_revision"

ensure_repository() {
	local docker_secret login_body login_response hub_token create_body status
	docker_secret=${registry_auth#*:}
	login_body=$(jq -nc \
		--arg username "$logged_in_user" \
		--arg password "$docker_secret" \
		'{username: $username, password: $password}')
	login_response=$(curl --fail --silent --show-error \
		--header 'Content-Type: application/json' \
		--data "$login_body" \
		https://hub.docker.com/v2/users/login/)
	hub_token=$(jq -er '.token' <<<"$login_response")
	status=$(curl --silent --show-error --output /dev/null --write-out '%{http_code}' \
		--header "Authorization: Bearer $hub_token" \
		"https://hub.docker.com/v2/namespaces/$dockerhub_namespace/repositories/$dockerhub_repository")
	if [[ $status == 200 ]]; then
		return
	fi
	if [[ $status != 404 ]]; then
		printf 'cannot inspect Docker Hub repository (HTTP %s)\n' "$status" >&2
		return 1
	fi
	create_body=$(jq -nc \
		--arg name "$dockerhub_repository" \
		--arg namespace "$dockerhub_namespace" \
		--argjson is_private "$dockerhub_private" \
		'{name: $name, namespace: $namespace, registry: "docker.io",
		  description: "PostgreSQL 19 with Chronos database branching",
		  is_private: $is_private}')
	status=$(curl --silent --show-error --output /dev/null --write-out '%{http_code}' \
		--request POST \
		--header "Authorization: Bearer $hub_token" \
		--header 'Content-Type: application/json' \
		--data "$create_body" \
		"https://hub.docker.com/v2/namespaces/$dockerhub_namespace/repositories")
	if [[ $status != 201 ]]; then
		printf 'cannot create Docker Hub repository (HTTP %s)\n' "$status" >&2
		return 1
	fi
}

docker build \
	--pull \
	--file "$repo_root/docker/chronos/Dockerfile" \
	--build-arg "VCS_REF=$revision" \
	--tag "$major_image" \
	--tag "$version_image" \
	--tag "$revision_image" \
	"$repo_root"

"$repo_root/docker/chronos/smoke-test.sh" "$revision_image"

ensure_repository
docker push "$revision_image"
docker push "$version_image"
docker push "$major_image"

printf 'published images:\n'
printf '  %s\n' "$revision_image" "$version_image" "$major_image"
