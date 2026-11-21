# Run the fast commit check; the commit hook runs this
test-commit:
    @just -f ~/ai-review-ci/justfiles/docs-and-configs.just -d . test-commit

# Run the fast push check; the push hook runs this
test-push:
    @just -f ~/ai-review-ci/justfiles/docs-and-configs.just -d . test-push
