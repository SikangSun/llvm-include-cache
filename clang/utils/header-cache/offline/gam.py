"""A penalized additive model of log write time (a generalized additive
model): for each value, a line in its log with bends at its quantiles, and
a ridge penalty on every bend and slope, so columns with few distinct
values cannot push the fit apart. The penalty is chosen by 3-fold
cross-validation inside the writes the fit sees; values beyond what the
fit saw are held at its edges."""
import numpy as np

def basis(F, lo, hi, knots):
    Fc = np.clip(F, lo, hi)
    cols = []
    for i in range(F.shape[1]):
        cols.append(Fc[:, i])
        for k in knots[i]:
            cols.append(np.maximum(0.0, Fc[:, i] - k))
    return np.column_stack(cols)

class GAM:
    def __init__(self, lams=(1e-3, 1e-2, 1e-1, 1, 10, 100)):
        self.lams = lams

    def _solve(self, X, y, lam):
        n, p = X.shape
        A = X.T @ X + lam * n * np.eye(p)
        return np.linalg.solve(A, X.T @ (y - y.mean())), y.mean()

    def _design(self, F):
        X = basis(F, self.lo, self.hi, self.knots)
        return (X - self.mu) / self.sd

    def fit(self, F, y, rng=None):
        self.lo, self.hi = F.min(0), F.max(0)
        self.knots = [np.unique(np.quantile(F[:, i], [0.2, 0.4, 0.6, 0.8])) for i in range(F.shape[1])]
        X = basis(F, self.lo, self.hi, self.knots)
        self.mu, self.sd = X.mean(0), X.std(0) + 1e-9
        X = (X - self.mu) / self.sd
        rng = rng or np.random.default_rng(0)
        idx = rng.permutation(len(y)); folds = np.array_split(idx, 3)
        best = None
        for lam in self.lams:
            err = 0.0
            for f in folds:
                tr = np.setdiff1d(idx, f)
                c, b = self._solve(X[tr], y[tr], lam)
                err += ((y[f] - (X[f] @ c + b)) ** 2).sum()
            if best is None or err < best[0]:
                best = (err, lam)
        self.lam = best[1]
        self.c, self.b = self._solve(X, y, self.lam)
        self.smear = np.mean(np.exp(y - (X @ self.c + self.b)))
        return self

    def predict_log(self, F):
        return self._design(F) @ self.c + self.b
