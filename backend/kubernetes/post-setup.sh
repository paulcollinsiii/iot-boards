#!/bin/sh

export KUBECONFIG=$(pwd)/admin.conf
k0sctl kubeconfig >| admin.conf
kubectl apply -k ./kustomize/env/dev && \
  kubectl wait --for=condition=ready certificate kaffi-homelab-ca && \
  kubectl get secret kaffi-homelab-ca -o jsonpath="{.data.ca\.crt}" | base64 -d - > ca.crt
vagrant ssh -- "sudo /bin/sh -c \"cp /vagrant/ca.crt /usr/local/share/ca-certificates && update-ca-certificates\""
helm install kaffi-registry ./charts/kaffi-registry
helm install eclipse-mqtt ./charts/eclipse-mqtt -n iot --create-namespace
